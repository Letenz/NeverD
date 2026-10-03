//===- DriverDormantGuardTests.cpp - Dormant PE CFG compatibility ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/windows/DriverImage.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <fstream>
#include <tuple>

namespace neverd::emulation {
namespace {
using Config = llvm::object::coff_load_configuration64;
#define NEVERD_GUARD_TEST_TEXT(Name, Value) constexpr char Name[] = Value;
#define NEVERD_GUARD_TEST_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "DriverGuardCases.def"
#undef NEVERD_GUARD_TEST_VALUE
#undef NEVERD_GUARD_TEST_TEXT

class DriverDormantGuardMutation : public testing::Test {
protected:
  std::vector<uint8_t> Bytes;
  std::unique_ptr<llvm::object::COFFObjectFile> Object;
  std::filesystem::path Directory;
  size_t ConfigOffset = 0;

  void SetUp() override {
    const auto Fixture =
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / DormantFixture;
    auto Buffer = llvm::MemoryBuffer::getFile(Fixture.string());
    ASSERT_TRUE(bool(Buffer)) << Buffer.getError().message();
    const auto Data = (*Buffer)->getBuffer();
    Bytes.assign(Data.bytes_begin(), Data.bytes_end());
    auto Parsed = llvm::object::COFFObjectFile::create(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                        Bytes.size()),
        SourceName));
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    Object = std::move(*Parsed);
    const auto *ConfigDirectory =
        Object->getDataDirectory(llvm::COFF::LOAD_CONFIG_TABLE);
    ASSERT_NE(ConfigDirectory, nullptr);
    ConfigOffset = rvaOffset(ConfigDirectory->RelativeVirtualAddress);
    llvm::support::endian::write32le(Bytes.data() + ConfigOffset +
                                         offsetof(Config, GuardFlags),
                                     DormantFlags);
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory(TemporaryPrefix, Temporary));
    Directory = Temporary.str().str();
  }
  void TearDown() override {
    std::error_code Ignored;
    if (!Directory.empty())
      std::filesystem::remove_all(Directory, Ignored);
  }
  size_t rvaOffset(uint64_t RVA) {
    uintptr_t Pointer = 0;
    auto E = Object->getRvaPtr(RVA, Pointer);
    EXPECT_FALSE(bool(E)) << llvm::toString(std::move(E));
    return Pointer ? reinterpret_cast<const uint8_t *>(Pointer) - Bytes.data()
                   : 0;
  }
  uint64_t field(size_t Offset) const {
    return llvm::support::endian::read64le(Bytes.data() + ConfigOffset +
                                           Offset);
  }
  std::filesystem::path writeImage() {
    const auto Path = Directory / DormantImage;
    std::ofstream Stream(Path, std::ios::binary);
    Stream.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Stream.close();
    EXPECT_TRUE(Stream);
    return Path;
  }
  void rejects(llvm::StringRef Diagnostic, uint64_t Base = 0) {
    auto Image =
        loadDriverImage(writeImage(), profile::DefaultMemoryLimit, Base);
    ASSERT_FALSE(bool(Image));
    const auto Reason = llvm::toString(Image.takeError());
    EXPECT_NE(Reason.find(Diagnostic.str()), std::string::npos) << Reason;
  }
};
TEST_F(DriverDormantGuardMutation, ZeroFlagsPreserveValidatedFallbackPointers) {
  const auto Path = writeImage();
  for (uint64_t Base : {Object->getImageBase(), RebasedAddress}) {
    auto Image = loadDriverImage(Path, profile::DefaultMemoryLimit, Base);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    EXPECT_FALSE(Image->Guard.Enabled);
    EXPECT_TRUE(Image->Guard.ValidTargets.empty());
    EXPECT_EQ(Image->Guard.CheckPointerAddress,
              Base + field(offsetof(Config, GuardCFCheckFunction)) -
                  Object->getImageBase());
    EXPECT_EQ(Image->Guard.DispatchPointerAddress,
              Base + field(offsetof(Config, GuardCFCheckDispatch)) -
                  Object->getImageBase());
    for (size_t Offset : {offsetof(Config, GuardCFCheckFunction),
                          offsetof(Config, GuardCFCheckDispatch)}) {
      const auto Slot = field(Offset) - Object->getImageBase();
      const auto Fallback =
          llvm::support::endian::read64le(Bytes.data() + rvaOffset(Slot));
      bool Found = false;
      for (const auto &Region : Image->Regions) {
        if (Base + Slot < Region.Address ||
            Base + Slot - Region.Address > Region.Bytes.size() ||
            sizeof(uint64_t) >
                Region.Bytes.size() - (Base + Slot - Region.Address))
          continue;
        EXPECT_EQ(llvm::support::endian::read64le(Region.Bytes.data() + Base +
                                                  Slot - Region.Address) -
                      Base,
                  Fallback - Object->getImageBase());
        Found = true;
      }
      EXPECT_TRUE(Found);
    }
  }
}
TEST_F(DriverDormantGuardMutation, ZeroFlagsStillRejectExternalPointerSlots) {
  llvm::support::endian::write64le(Bytes.data() + ConfigOffset +
                                       offsetof(Config, GuardCFCheckFunction),
                                   ExternalAddress);
  rejects(ExternalSlot);
}
TEST_F(DriverDormantGuardMutation, ZeroFlagsStillRejectExternalFallbackCode) {
  const auto Offset = rvaOffset(field(offsetof(Config, GuardCFCheckFunction)) -
                                Object->getImageBase());
  llvm::support::endian::write64le(Bytes.data() + Offset, ExternalAddress);
  rejects(ExternalFallback);
}
TEST_F(DriverDormantGuardMutation, ZeroFlagsStillRequirePointerRelocations) {
  const auto *Relocations =
      Object->getDataDirectory(llvm::COFF::BASE_RELOCATION_TABLE);
  ASSERT_NE(Relocations, nullptr);
  size_t Offset = rvaOffset(Relocations->RelativeVirtualAddress);
  const size_t End = Offset + Relocations->Size;
  const auto Slot =
      field(offsetof(Config, GuardCFCheckFunction)) - Object->getImageBase();
  bool Replaced = false;
  while (Offset + sizeof(llvm::object::coff_base_reloc_block_header) <= End) {
    const uint32_t Page =
        llvm::support::endian::read32le(Bytes.data() + Offset);
    const uint32_t Size = llvm::support::endian::read32le(
        Bytes.data() + Offset + sizeof(uint32_t));
    ASSERT_GE(Size, sizeof(llvm::object::coff_base_reloc_block_header));
    ASSERT_LE(Size, End - Offset);
    for (size_t Position =
             Offset + sizeof(llvm::object::coff_base_reloc_block_header);
         Position + sizeof(uint16_t) <= Offset + Size;
         Position += sizeof(uint16_t)) {
      const uint16_t Entry =
          llvm::support::endian::read16le(Bytes.data() + Position);
      if ((Entry >> RelocationTypeShift) != llvm::COFF::IMAGE_REL_BASED_DIR64 ||
          Page + (Entry & RelocationOffsetMask) != Slot)
        continue;
      llvm::support::endian::write16le(Bytes.data() + Position,
                                       llvm::COFF::IMAGE_REL_BASED_ABSOLUTE);
      Replaced = true;
    }
    Offset += Size;
  }
  ASSERT_TRUE(Replaced) << MissingFixtureRelocation;
  rejects(MissingRelocation, RebasedAddress);
}
using ExecutionParameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
class DriverDormantGuardExecution
    : public DriverDormantGuardMutation,
      public testing::WithParamInterface<ExecutionParameter> {};
TEST_P(DriverDormantGuardExecution, ZeroFlagsExecuteOriginalWin64Fallbacks) {
  const auto [Backend, Contract] = GetParam();
  auto Probe =
      createExecutionBackend(Backend, Contract, profile::DefaultMemoryLimit);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable && !requireHvf(Backend, GuestArchitecture::X64))
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  const auto Path = writeImage();
  for (uint64_t Base : {Object->getImageBase(), RebasedAddress}) {
    DriverOptions Options;
    Options.Backend = Backend;
    Options.Contract = Contract;
    Options.LoadAddress = Base;
    auto Result = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_TRUE(Result->NTStatus);
    EXPECT_EQ(*Result->NTStatus, 0u);
  }
}
INSTANTIATE_TEST_SUITE_P(
    Backends, DriverDormantGuardExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64)));
} // namespace
} // namespace neverd::emulation

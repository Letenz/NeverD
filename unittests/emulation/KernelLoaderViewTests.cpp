//===- KernelLoaderViewTests.cpp - Concrete loader record boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently specified byte, ownership and declaration controls.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelModel.h"

#include "neverd/emulation/AddressSpace.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
constexpr uint64_t Scratch = 0x60000000;
constexpr uint64_t MemoryLimit = 16 * 1024 * 1024;

struct Session {
  std::shared_ptr<AddressSpace> Memory = llvm::cantFail(AddressSpace::create(
      llvm::cantFail(PhysicalMemory::create(MemoryLimit)), MemoryLimit));
  DriverResult Result;
  KernelModel Model{*Memory, Result};
  DriverImage Image;
  Session() {
    llvm::cantFail(Memory->map(Scratch, 4096, Read | Write));
    Image.Base = 0x180000000;
    Image.Entry = Image.Base + 0x1000;
    Image.Size = 0x4000;
  }
  void initialize(std::optional<DriverLoaderView> View = std::nullopt) {
    DriverOptions Options;
    Options.LoaderView = std::move(View);
    llvm::cantFail(Model.initialize(Image, Options));
  }
  uint64_t integer(uint64_t Address, unsigned Size = 8) {
    return llvm::cantFail(Memory->readInteger(Address, Size));
  }
  uint64_t record() { return integer(Model.driverObject() + 40); }
  uint64_t call(llvm::StringRef Name,
                std::initializer_list<uint64_t> Arguments) {
    return llvm::cantFail(Model.call(Name.str(), Arguments));
  }
  std::vector<uint8_t> bytes(uint64_t Address, size_t Size) {
    std::vector<uint8_t> Out(Size);
    llvm::cantFail(Memory->read(Address, Out));
    return Out;
  }
};

void denied(llvm::Error E, llvm::StringRef Text) {
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()), std::string::npos);
}
template <typename T> void denied(llvm::Expected<T> Value) {
  ASSERT_FALSE(bool(Value));
  EXPECT_FALSE(llvm::toString(Value.takeError()).empty());
}

TEST(KernelLoaderView, DefaultSectionAndFlagsStayOpaque) {
  Session S;
  S.initialize();
  for (unsigned Offset : {16u, 40u, 47u})
    denied(
        S.Model.validateGuestAccess(S.Model.driverObject() + Offset, 1, false),
        "unmodeled");
  llvm::cantFail(S.Model.captureUnpackBaseline());
  EXPECT_TRUE(llvm::cantFail(S.Model.unpackDependencies()).empty());
}

TEST(KernelLoaderView, EveryByteAndStraddlingFootprintAreChecked) {
  Session S;
  S.initialize(DriverLoaderView{1, 0x93a57cf1, "independent byte fixture"});
  const auto Record = S.record();
  ASSERT_NE(Record, 0u);
  llvm::cantFail(
      S.Model.validateGuestAccess(S.Model.driverObject() + 40, 8, false));
  for (unsigned Byte = 0; Byte != 160; ++Byte) {
    SCOPED_TRACE(Byte);
    // This oracle is intentionally independent of the implementation layout.
    const bool Readable =
        (Byte >= 48 && Byte < 68) || (Byte >= 104 && Byte < 108);
    auto E = S.Model.validateGuestAccess(Record + Byte, 1, false);
    EXPECT_EQ(bool(E), !Readable);
    llvm::consumeError(std::move(E));
    denied(S.Model.validateGuestAccess(Record + Byte, 1, true), "read-only");
  }
  for (const auto [Offset, Size] :
       {std::pair{47u, 2u}, {64u, 8u}, {103u, 2u}, {104u, 8u}, {107u, 2u}})
    denied(S.Model.validateGuestAccess(Record + Offset, Size, false),
           "unmodeled");
  for (unsigned Offset : {16u, 40u})
    denied(
        S.Model.validateGuestAccess(S.Model.driverObject() + Offset, 4, true),
        "read-only");
  denied(S.Model.validateGuestAccess(S.Model.driverObject() + 16, 4, false),
         "unmodeled");
  EXPECT_EQ(S.integer(Record + 105, 2), 0xa57cu);
  EXPECT_EQ(S.integer(Record + 107, 1), 0x93u);
}

TEST(KernelLoaderView, ImageMetadataAndExplicitFlagsStaySessionLocal) {
  Session A, B;
  A.initialize(DriverLoaderView{1, 0, "zero is explicitly supplied"});
  B.Image.Base = 0x1a0000000;
  B.Image.Entry = B.Image.Base + 0x2100;
  B.Image.Size = 0x7000;
  B.initialize(DriverLoaderView{1, UINT32_MAX, "all bits explicitly supplied"});
  for (auto *S : {&A, &B}) {
    EXPECT_EQ(S->integer(S->record() + 48), S->Image.Base);
    EXPECT_EQ(S->integer(S->record() + 56), S->Image.Entry);
    EXPECT_EQ(S->integer(S->record() + 64, 4), S->Image.Size);
  }
  // Guest addresses may be equal. Their storage and dependencies must not
  // alias.
  EXPECT_EQ(A.integer(A.record() + 104, 4), 0u);
  EXPECT_EQ(B.integer(B.record() + 104, 4), UINT32_MAX);
  llvm::cantFail(B.Memory->writeInteger(Scratch, 0x91, 1));
  EXPECT_EQ(A.integer(Scratch, 1), 0u);
  EXPECT_EQ(B.integer(Scratch, 1), 0x91u);
  Session Default;
  Default.initialize();
  llvm::cantFail(Default.Model.captureUnpackBaseline());
  EXPECT_TRUE(llvm::cantFail(Default.Model.unpackDependencies()).empty());
}

TEST(KernelLoaderView, CopyServicesAndMDLsCannotBypassBorrowedReadOnlyState) {
  Session S;
  S.initialize(DriverLoaderView{1, 0x87654321, "copy and mapping controls"});
  const auto Record = S.record();
  const auto Before = S.bytes(Record, 160);
  S.call("memcpy", {Scratch, Record + 104, 4});
  EXPECT_EQ(S.integer(Scratch, 4), 0x87654321u);
  const auto Output = S.bytes(Scratch, 160);
  denied(S.Model.call("memcpy", {Scratch, Record + 64, 8}));
  EXPECT_EQ(S.bytes(Scratch, 160), Output);
  denied(S.Model.call("memcpy", {Record + 104, Scratch, 4}));
  denied(S.Model.call("RtlZeroMemory", {Record, 160}));
  denied(S.Model.call("RtlFillMemory", {Record + 48, 8, 0xff}));
  for (const auto [Offset, Size] : {std::pair{0u, 160u}, {104u, 4u}}) {
    const auto MDL = S.call("IoAllocateMdl", {Record + Offset, Size, 0, 0, 0});
    ASSERT_NE(MDL, 0u);
    denied(S.Model.call("MmBuildMdlForNonPagedPool", {MDL}));
    denied(S.Model.call("MmProbeAndLockPages", {MDL, 0, 0}));
    denied(S.Model.call("MmProbeAndLockPages", {MDL, 0, 1}));
    denied(S.Model.call("MmMapLockedPagesSpecifyCache", {MDL, 0, 1, 0, 0, 0}));
    S.call("IoFreeMdl", {MDL});
  }
  EXPECT_EQ(S.bytes(Record, 160), Before);
}

TEST(KernelLoaderView, UnusedDeclarationRetainsDependencyAndBorrowedLifetime) {
  Session S;
  S.initialize(DriverLoaderView{1, 0, "unused declaration"});
  llvm::cantFail(S.Model.captureUnpackBaseline());
  // No guest access is required for this dependency to be retained.
  auto State = llvm::cantFail(S.Model.captureUnpackRuntime(true));
  EXPECT_TRUE(State.hasAdditionalDependencies());
  EXPECT_TRUE(State.Pools.empty());
  EXPECT_EQ(State.Reasons,
            (std::vector<std::string>{
                "unreconstructed kernel or environment effects"}));
  const auto Record = S.record();
  EXPECT_EQ(S.Model.unpackAllocations().at(Record), 160u);
  llvm::cantFail(
      S.Memory->writeInteger(S.Model.driverObject() + 104, S.Image.Entry, 8));
  llvm::cantFail(S.Model.finishEntry());
  llvm::cantFail(S.Model.validateGuestAccess(Record + 104, 4, false));
  auto Unload = S.Model.beginUnload();
  ASSERT_TRUE(bool(Unload)) << llvm::toString(Unload.takeError());
  llvm::cantFail(S.Model.validateGuestAccess(Record + 48, 20, false));
  EXPECT_EQ(S.Model.unpackAllocations().at(Record), 160u);
  const auto Later = llvm::cantFail(S.Model.captureUnpackRuntime(false));
  EXPECT_NE(std::find(Later.Reasons.begin(), Later.Reasons.end(),
                      State.Reasons.front()),
            Later.Reasons.end());
  EXPECT_EQ(State.Reasons.size(), 1u);
}

TEST(KernelLoaderView, InvalidDirectDeclarationsFailBeforeAllocation) {
  for (const auto &View : {DriverLoaderView{0, 0, "unsupported"},
                           DriverLoaderView{2, 0, "unsupported"},
                           DriverLoaderView{1, std::nullopt, "missing flags"},
                           DriverLoaderView{1, 0, ""},
                           DriverLoaderView{1, 0, std::string(257, 'a')},
                           DriverLoaderView{1, 0, "embedded\ncontrol"}}) {
    Session S;
    DriverOptions Options;
    Options.LoaderView = View;
    denied(S.Model.initialize(S.Image, Options), "loader_view");
    EXPECT_EQ(S.Model.driverObject(), 0u);
    EXPECT_TRUE(S.Model.unpackAllocations().empty());
    EXPECT_FALSE(
        llvm::cantFail(S.Memory->canAccess(profile::KernelArenaBase, 1, Read)));
    // The same model can still accept a valid first initialization.
    S.initialize(DriverLoaderView{1, 0, "valid retry"});
    EXPECT_NE(S.Model.driverObject(), 0u);
  }
}

TEST(KernelLoaderView, InvalidImageMetadataCannotBeTruncatedIntoTheView) {
  for (unsigned Case = 0; Case != 4; ++Case) {
    Session S;
    if (Case == 0)
      S.Image.Size = uint64_t(UINT32_MAX) + 1;
    else if (Case == 1)
      S.Image.Size = 0;
    else if (Case == 2)
      S.Image.Entry = S.Image.Base + S.Image.Size;
    else
      S.Image.Base = UINT64_MAX - 1;
    DriverOptions Options;
    Options.LoaderView = DriverLoaderView{1, 0, "invalid image control"};
    denied(S.Model.initialize(S.Image, Options), "image metadata");
    EXPECT_TRUE(S.Model.unpackAllocations().empty());
  }
}
} // namespace
} // namespace neverd::emulation

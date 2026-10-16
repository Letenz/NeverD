//===- DarwinDylibTests.cpp - Startup graph and TLS admission boundaries
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Original-byte mutations and aggregate budgets for explicit dylibs.
#include "DarwinTestImage.h"
#include "gtest/gtest.h"
#include "os/darwin/process/DarwinProcess.h"

#include "neverd/emulation/ProcessReport.h"
#include "neverd/loader/MachO/MachOExecutionImage.h"

#include "llvm/Support/MemoryBuffer.h"
namespace neverd::emulation {
namespace {
using namespace llvm::MachO;
using namespace llvm::support::endian;
using darwin_test::Image;
using darwin_test::TemporaryImage;
class DarwinDylib : public testing::Test {
protected:
  std::filesystem::path Directory;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_DARWIN_FIXTURE_DIR
    GTEST_SKIP() << "Darwin compiler fixtures unavailable";
#else
    Directory = std::filesystem::path(NEVERD_DARWIN_FIXTURE_DIR) /
                "macos-arm64-dylib-classic";
    Options.StackSize = 16384;
    Options.Darwin =
        DarwinProcessOptions{{{"@loader_path/libneverd-base.dylib",
                               Directory / "libneverd-base.dylib"},
                              {"@loader_path/libneverd-leaf.dylib",
                               Directory / "libneverd-leaf.dylib"}}};
#endif
  }
  Image bytes(llvm::StringRef Name) {
    auto Buffer =
        llvm::MemoryBuffer::getFile((Directory / Name.str()).string());
    EXPECT_TRUE(bool(Buffer));
    Image I;
    if (Buffer)
      I.Bytes.assign((*Buffer)->getBufferStart(), (*Buffer)->getBufferEnd());
    return I;
  }
  std::vector<size_t> commands(const Image &I, uint32_t Kind) {
    std::vector<size_t> Out;
    size_t At = 32;
    for (unsigned N = read32le(I.Bytes.data() + 16); N; --N) {
      if (read32le(I.Bytes.data() + At) == Kind)
        Out.push_back(At);
      At += read32le(I.Bytes.data() + At + 4);
    }
    return Out;
  }
  size_t section(const Image &I, uint32_t Kind) {
    for (auto C : commands(I, LC_SEGMENT_64))
      for (unsigned N = read32le(I.Bytes.data() + C + 64), S = 0; S < N; ++S) {
        const auto At = C + 72 + S * 80;
        if ((read32le(I.Bytes.data() + At + 64) & SECTION_TYPE) == Kind)
          return At;
      }
    ADD_FAILURE() << "original fixture lacks requested section";
    return 0;
  }
  auto load() {
    return darwin_model::loadImage(Directory / "main",
                                   darwin_model::macOSProfile(), Options);
  }
  void rejects(const char *Contains = nullptr) {
    auto R = load();
    ASSERT_FALSE(bool(R));
    const auto Text = llvm::toString(R.takeError());
    if (Contains)
      EXPECT_NE(Text.find(Contains), std::string::npos) << Text;
  }
};
TEST_F(DarwinDylib, SharedDependencyConsumesItsCompleteInputOnlyOnce) {
  auto Base = bytes("libneverd-base.dylib"),
       Leaf = bytes("libneverd-leaf.dylib");
  Base.Bytes.resize(Base.Bytes.size() + 1024 * 1024);
  Leaf.Bytes.resize(Leaf.Bytes.size() + 1024 * 1024);
  TemporaryImage B(Base), L(Leaf);
  Options.Darwin->Modules[0].Path = B.path();
  Options.Darwin->Modules[1].Path = L.path();
  const uint64_t Total =
      Base.Bytes.size() + Leaf.Bytes.size() + bytes("main").Bytes.size();
  Options.MemoryLimit = Total - 1;
  rejects("file byte limit");
  Options.MemoryLimit = Total;
  auto R = load();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  EXPECT_EQ(R->TLS.size(), 3u);
  EXPECT_EQ(R->TLSDescriptors.size(), 5u);
}
TEST_F(DarwinDylib, ResidentBudgetIncludesAllImagesTLSAndProviderReservations) {
  auto First = load();
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  Options.MemoryLimit = First->MetadataBytes + First->Plan.MappedBytes +
                        Options.StackSize + 3 * First->Memory.PageSize;
  auto Exact = load();
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  --Options.MemoryLimit;
  rejects();
}
TEST_F(DarwinDylib, CatalogueRejectsMissingDuplicateReservedAndWrongProfile) {
  Options.Darwin->Modules.pop_back();
  rejects("provider");
  SetUp();
  Options.Darwin->Modules.push_back(Options.Darwin->Modules.front());
  rejects("catalogue");
  SetUp();
  Options.Darwin->Modules.front().Name = "/usr/lib/libSystem.B.dylib";
  rejects("catalogue");
  auto Other =
      emulateProcess(Directory / "main", ProcessProfile::LinuxELF64, Options);
  ASSERT_FALSE(bool(Other));
  EXPECT_NE(llvm::toString(Other.takeError()).find("Darwin profile"),
            std::string::npos);
  for (const char *JSON :
       {R"({"darwin":null})", R"({"darwin":{"unknown":[]}})",
        R"({"darwin":{"modules":{}}})",
        R"({"darwin":{"modules":[{"name":"x","path":""}]}})",
        R"({"darwin":{"modules":[{"name":"x","path":"p","extra":1}]}})"}) {
    auto R = processOptionsFromJSON(JSON);
    EXPECT_FALSE(bool(R)) << JSON;
    llvm::consumeError(R.takeError());
  }
  auto Parsed = processOptionsFromJSON(
      R"({"darwin":{"modules":[{"name":"@loader_path/example","path":"unused"}]}})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->Darwin);
  EXPECT_EQ(Parsed->Darwin->Modules.front().Name, "@loader_path/example");
}
TEST_F(DarwinDylib, EveryReferenceChecksIdentityVersionISAAndPlatform) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto I = bytes("libneverd-base.dylib");
    if (Mutation < 2) {
      const auto ID = commands(I, LC_ID_DYLIB).front();
      if (!Mutation)
        I.Bytes[ID + read32le(I.Bytes.data() + ID + 8)] = 'X';
      else
        I.u32(ID + 16, 0);
    } else if (Mutation == 2) {
      I.u32(4, CPU_TYPE_X86_64);
      I.u32(8, CPU_SUBTYPE_X86_64_ALL);
    } else
      I.u32(commands(I, LC_BUILD_VERSION).front() + 8, PLATFORM_IOS);
    TemporaryImage File(I);
    Options.Darwin->Modules[0].Path = File.path();
    rejects();
  }
  // Main already loaded the shared base with compatibility 1.0. The second
  // edge must still enforce the leaf's independent, stronger requirement.
  SetUp();
  auto Leaf = bytes("libneverd-leaf.dylib");
  for (const auto D : commands(Leaf, LC_LOAD_DYLIB)) {
    const auto Offset = D + read32le(Leaf.Bytes.data() + D + 8);
    if (std::string(reinterpret_cast<const char *>(
            Leaf.Bytes.data() + Offset)) == "@loader_path/libneverd-base.dylib")
      Leaf.u32(D + 20, 0x20000);
  }
  TemporaryImage File(Leaf);
  Options.Darwin->Modules[1].Path = File.path();
  rejects("compatibility version");
}
TEST_F(DarwinDylib, SelfDependencyAndExpiredPreparationNeverStartGuestCode) {
  auto I = bytes("libneverd-leaf.dylib");
  for (auto D : commands(I, LC_LOAD_DYLIB)) {
    const auto Offset = D + read32le(I.Bytes.data() + D + 8);
    if (std::string(reinterpret_cast<const char *>(I.Bytes.data() + Offset)) ==
        "@loader_path/libneverd-base.dylib") {
      const char Name[] = "@loader_path/libneverd-leaf.dylib";
      std::copy(std::begin(Name), std::end(Name), I.Bytes.begin() + Offset);
    }
  }
  TemporaryImage File(I);
  Options.Darwin->Modules[1].Path = File.path();
  rejects("cycle");
  SetUp();
  auto Budget = ExecutionBudget::create(
      Options.Limits, ExecutionBudget::Clock::now() - std::chrono::seconds(10));
  ASSERT_TRUE(bool(Budget));
  auto R = darwin_model::loadImage(
      Directory / "main", darwin_model::macOSProfile(), Options, Budget->get());
  ASSERT_FALSE(bool(R));
  bool Timeout = false;
  auto Other = llvm::handleErrors(
      R.takeError(),
      [&](darwin_model::PreparationTimeout &) { Timeout = true; });
  EXPECT_FALSE(bool(Other)) << llvm::toString(std::move(Other));
  EXPECT_TRUE(Timeout);
}
TEST_F(DarwinDylib, InvalidTLSLayoutsFailBeforeAnyExecution) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto I = bytes("libneverd-leaf.dylib");
    const auto Descriptor = section(I, S_THREAD_LOCAL_VARIABLES);
    const auto Data = section(I, S_THREAD_LOCAL_REGULAR);
    const auto File = read32le(I.Bytes.data() + Descriptor + 48);
    switch (Mutation) {
    case 0:
      I.u64(File + 8, 9);
      break;
    case 1:
      I.u64(File + 16, UINT64_MAX);
      break;
    case 2:
      I.u64(Descriptor + 40, 23);
      break;
    case 3:
      I.u32(Data + 52, 6);
      break;
    case 4:
      I.u32(Data + 64, S_THREAD_LOCAL_INIT_FUNCTION_POINTERS);
      break;
    case 5:
      I.u32(24, read32le(I.Bytes.data() + 24) & ~MH_HAS_TLV_DESCRIPTORS);
      break;
    }
    TemporaryImage FileImage(I);
    Options.Darwin->Modules[1].Path = FileImage.path();
    rejects("TLS");
  }
}
} // namespace
} // namespace neverd::emulation

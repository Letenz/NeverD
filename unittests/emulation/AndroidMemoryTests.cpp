//===- AndroidMemoryTests.cpp - Explicit local memory inputs -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000, Extent = 18 * 4096;
class AndroidMemoryInput : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Image, Root, Input;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Image = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
            (std::string(GetParam()) + ".so");
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-memory", Directory));
    Root = Directory.str().str();
    Input = Root / std::filesystem::path(u8"input-数据.bin");
    Options.Backend = ExecutionBackendKind::Unicorn;
    Options.Limits.Instructions = 1000000;
    Options.Limits.TimeoutMicroseconds = 20000000;
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->EntrySymbol = "inspect_memory_input";
    Options.Android->Memory.push_back({Buffer, Extent, {}, false, Input});
    Options.Android->ReadMemory.push_back({Buffer, Extent});
#endif
  }
  void TearDown() override {
    if (!Root.empty())
      std::filesystem::remove_all(Root);
  }
  void write(llvm::ArrayRef<uint8_t> Bytes) {
    auto Name = Input.u8string();
    std::error_code Error;
    llvm::raw_fd_ostream OS(
        llvm::StringRef(reinterpret_cast<const char *>(Name.data()),
                        Name.size()),
        Error);
    ASSERT_FALSE(Error) << Error.message();
    OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    OS.close();
    ASSERT_FALSE(OS.has_error());
  }
  auto run() {
    return emulateProcess(Image, ProcessProfile::AndroidNativeAArch64, Options);
  }
};

TEST_P(AndroidMemoryInput, CopiesWholeFileAndPaddingWithoutHostWriteback) {
  std::vector<uint8_t> Bytes(65539);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = static_cast<uint8_t>(I * 37 + (I >> 8));
  Options.Android->Arguments = {Buffer, Bytes.size()};
  for (unsigned Run = 0; Run < 2; ++Run) {
    SCOPED_TRACE(Run);
    if (Run)
      Bytes.back() ^= 127;
    write(Bytes);
    uint64_t Expected = 14695981039346656037ull;
    for (size_t I = 0; I < Bytes.size();) {
      uint64_t Word = 0;
      size_t Width = Bytes.size() - I >= 8 ? 8 : 1;
      for (size_t J = 0; J < Width; ++J)
        Word |= uint64_t(Bytes[I++]) << (8 * J);
      Expected = (Expected ^ Word) * 1099511628211ull;
    }
    auto R = run();
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    ASSERT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->ReturnValue, Expected);
    auto Mutated = Bytes;
    Mutated.front() ^= 255;
    Mutated.push_back(165);
    Mutated.resize(Extent, 0);
    ASSERT_EQ(R->MemorySnapshots.size(), 1u);
    EXPECT_EQ(R->MemorySnapshots[0].Bytes, Mutated);
    auto Name = Input.u8string();
    auto Original = llvm::MemoryBuffer::getFile(llvm::StringRef(
        reinterpret_cast<const char *>(Name.data()), Name.size()));
    ASSERT_TRUE(bool(Original));
    EXPECT_EQ((*Original)->getBuffer(),
              llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                              Bytes.size()));
  }
}

TEST_P(AndroidMemoryInput, EmptyFileStartsZeroed) {
  write({});
  Options.Android->Arguments = {Buffer, 0};
  auto R = run();
  ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
  ASSERT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
  EXPECT_EQ(R->ReturnValue, 14695981039346656037ull);
  std::vector<uint8_t> Expected(Extent, 0);
  Expected[0] = 165;
  ASSERT_EQ(R->MemorySnapshots.size(), 1u);
  EXPECT_EQ(R->MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidMemoryInput, RejectsInvalidFilesAndConflictingInitializers) {
  auto Original = Options;
  auto Rejected = [&](llvm::StringRef Message) {
    auto R = run();
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find(Message.str()),
              std::string::npos);
    Options = Original;
  };
  Rejected("memory input file I/O failed");
  Options.Android->Memory[0].File = Root;
  Rejected("must be a regular file");
  Options.Android->Memory[0].File = std::filesystem::path();
  Rejected("invalid memory input path");
  Options.Android->Memory[0].File =
      std::filesystem::path(std::string("a\0b", 3));
  Rejected("invalid memory input path");
  write({1, 2, 3});
  Options.Android->Memory[0].Bytes = {4};
  Rejected("mutually exclusive");
  write(std::vector<uint8_t>(Extent + 1));
  Rejected("file exceeds its region");
#ifndef _WIN32
  auto Pipe = Root / "pipe";
  ASSERT_EQ(::mkfifo(Pipe.c_str(), 0600), 0);
  Options.Android->Memory[0].File = Pipe;
  Rejected("must be a regular file");
#endif
}

TEST_P(AndroidMemoryInput, InputMappingSharesAggregateMemoryBudget) {
  write({1});
  Options.Android->Memory[0].Size = Options.MemoryLimit;
  auto R = run();
  ASSERT_FALSE(bool(R));
  // The image and stack already consume the same budget, before file I/O.
  EXPECT_NE(llvm::toString(R.takeError()).find("limit"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(Relocations, AndroidMemoryInput,
                         testing::Values("none", "android", "relr"));

TEST(AndroidMemoryInput, ExpiredPreparationDoesNotReadOrChangeGuestMemory) {
  auto Physical = llvm::cantFail(PhysicalMemory::create(4096));
  auto Space = llvm::cantFail(AddressSpace::create(Physical, 4096));
  llvm::cantFail(Space->map(Buffer, 4096, Read | Write | UserAccessible));
  NativeMemoryRegion Region{Buffer, 4096, {}, false, "not-opened.bin"};
  auto Budget = llvm::cantFail(ExecutionBudget::create(
      {1, 1, 1}, ExecutionBudget::Clock::time_point::min()));
  auto E = android_model::initializeMemoryRegion(*Space, Region, *Budget);
  ASSERT_TRUE(bool(E));
  EXPECT_NE(llvm::toString(std::move(E)).find("preparation deadline expired"),
            std::string::npos);
  std::vector<uint8_t> Bytes(4096, 255);
  llvm::cantFail(Space->read(Buffer, Bytes));
  EXPECT_EQ(Bytes, std::vector<uint8_t>(4096, 0));
}

TEST(AndroidMemoryInput, JSONPreservesPathsAndRejectsAmbiguousInputs) {
  auto O = processOptionsFromJSON(
      R"({"android":{"memory":[{"address":4096,"size":4096,"path":"input-\u6570\u636e.bin"}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Android);
  ASSERT_EQ(O->Android->Memory.size(), 1u);
  EXPECT_EQ(O->Android->Memory[0].File,
            std::filesystem::path(u8"input-数据.bin"));
  for (
      const char *Text :
      {R"({"android":{"memory":[{"address":4096,"size":4096,"path":"a","bytes_hex":""}]}})",
       R"({"android":{"memory":[{"address":4096,"size":4096,"path":""}]}})",
       R"({"android":{"memory":[{"address":4096,"size":4096,"path":null}]}})",
       R"({"android":{"memory":[{"address":4096,"size":4096,"path":"a\u0000b"}]}})",
       R"({"android":{"read_memory":[{"address":4096,"size":8,"path":"a"}]}})"}) {
    SCOPED_TRACE(Text);
    auto Invalid = processOptionsFromJSON(Text);
    ASSERT_FALSE(bool(Invalid));
    EXPECT_NE(llvm::toString(Invalid.takeError()).find("path"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation

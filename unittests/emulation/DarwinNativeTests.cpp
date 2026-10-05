//===- DarwinNativeTests.cpp - Original workloads on the host kernel -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#if defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace neverd::emulation {
namespace {
TEST(DarwinNative, Stat64WireRecordMatchesHostSDKAndFilesystemObservation) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "Darwin SDK and native filesystem observation require macOS";
#else
  using namespace darwin_model;
  struct stat Native{};
  ASSERT_EQ(sizeof(Native), 144u);
#define NEVERD_DARWIN_FILE_STATUS(Member, Host, Offset, Width)                 \
  EXPECT_EQ(offsetof(struct stat, Host), Offset##u);                           \
  EXPECT_EQ(sizeof(Native.Host), Width##u);
#include "os/darwin/kernel/DarwinFileStatus.def"
#undef NEVERD_DARWIN_FILE_STATUS
  EXPECT_EQ(offsetof(struct stat, st_rdev), 24u);
  EXPECT_EQ(offsetof(struct stat, st_lspare), 124u);
  EXPECT_EQ(offsetof(struct stat, st_qspare), 128u);
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-stat", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / "data").string();
  {
    std::ofstream File(Input, std::ios::binary);
    File << "0123456789";
    ASSERT_TRUE(File.good());
  }
  ASSERT_EQ(::stat(Input.c_str(), &Native), 0);
  std::optional<DarwinFileOptions> Options(std::in_place);
  Options->Files["/data"] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  auto &M = Options->Metadata["/data"];
#define NEVERD_DARWIN_FILE_STATUS(Member, Host, Offset, Width)                 \
  M.Member = Native.Host;
#include "os/darwin/kernel/DarwinFileStatus.def"
#undef NEVERD_DARWIN_FILE_STATUS
  ASSERT_FALSE(bool(validateFileOptions(*Options)));
  auto Physical = PhysicalMemory::create(16384);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, 16384);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  const uint64_t Base = 0x100000;
  ASSERT_FALSE(bool((*Space)->map(Base, 4096, Read | Write | UserAccessible)));
  const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
  ASSERT_FALSE(bool((*Space)->write(Base, Path)));
  DarwinFiles Files(**Space, Options);
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn, "native ABI comparison"};
  auto Returned = Files.handle(
      ServiceKind::Stat64, {0, 338, {Base, Base + 256}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Returned)) << llvm::toString(Returned.takeError());
  ASSERT_TRUE(Returned->has_value()) << Result.Diagnostic;
  ASSERT_FALSE((**Returned).Error);
  std::array<uint8_t, 144> Bytes;
  ASSERT_FALSE(bool((*Space)->read(Base + 256, Bytes)));
  EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes),
            llvm::ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(&Native),
                                    sizeof(Native)));
#endif
}
TEST(DarwinNative, OriginalMemoryAndWriteContractsMatchHostKernel) {
#ifndef NEVERD_DARWIN_NATIVE_ORACLE
#if defined(__APPLE__)
  if (std::getenv("NEVERD_REQUIRE_HVF"))
    FAIL() << "native Darwin reference executable is required";
#endif
  GTEST_SKIP() << "native Darwin reference requires a macOS build host";
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-native", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const std::string Program = NEVERD_DARWIN_NATIVE_ORACLE;
  const auto Input = (Root / "data").string();
  {
    std::ofstream File(Input, std::ios::binary);
    File << "0123456789";
    ASSERT_TRUE(File.good());
  }
  struct Case {
    const char *Mode;
    int Status;
    const char *Output;
  };
  constexpr Case Cases[] = {
#define NEVERD_DARWIN_NATIVE_CASE(Mode, Status, Output) {Mode, Status, Output},
#include "fixtures/DarwinNativeCases.def"
#undef NEVERD_DARWIN_NATIVE_CASE
  };
  static_assert(std::size(Cases) != 0);
  for (const auto &Test : Cases) {
    SCOPED_TRACE(Test.Mode);
    // ExecuteAndWait does not truncate an existing redirection target on
    // every host. Keep each observation separate, including shorter outputs.
    const auto Output = (Root / (std::string(Test.Mode) + ".stdout")).string();
    const auto Error = (Root / (std::string(Test.Mode) + ".stderr")).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string LaunchError;
    bool ExecutionFailed = false;
    const auto Status = llvm::sys::ExecuteAndWait(
        Program, {Program, Test.Mode, Input}, std::nullopt, Redirects, 5, 0,
        &LaunchError, &ExecutionFailed);
    ASSERT_FALSE(ExecutionFailed) << LaunchError;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Out));
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Err));
    EXPECT_EQ(Status, Test.Status) << LaunchError << (*Err)->getBuffer().str();
    EXPECT_TRUE((*Err)->getBuffer().empty());
    EXPECT_EQ((*Out)->getBuffer(), Test.Output);
  }
#endif
}
} // namespace
} // namespace neverd::emulation

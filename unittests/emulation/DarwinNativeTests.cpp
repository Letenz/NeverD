//===- DarwinNativeTests.cpp - Original workloads on the host kernel -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/darwin/kernel/DarwinFiles.h"
#include "os/darwin/kernel/DarwinMemory.h"

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
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neverd::emulation {
namespace {
TEST(DarwinNative, PrivateFileOffsetAndTailMatchHostMapping) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "native Darwin file mappings require macOS";
#else
  using namespace darwin_model;
  const uint64_t Page = ::getpagesize();
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-darwin-mapping", Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  const auto Input = (Root / "data").string();
  std::vector<uint8_t> Bytes(Page + 19);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I * 17 + 5);
  {
    std::ofstream File(Input, std::ios::binary);
    File.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    ASSERT_TRUE(File.good());
  }
  const int NativeFD = ::open(Input.c_str(), O_RDONLY);
  ASSERT_GE(NativeFD, 0);
  // The host permits the extra EOF page at mmap time, then raises SIGBUS on
  // access. The bounded model admits only the first, partially filled page.
  void *Native = ::mmap(nullptr, Page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE,
                        NativeFD, Page);
  ASSERT_EQ(::close(NativeFD), 0);
  ASSERT_NE(Native, MAP_FAILED);
  auto Unmap = llvm::scope_exit([&] { ::munmap(Native, Page * 2); });
  auto Physical = PhysicalMemory::create(Page * 2);
  ASSERT_TRUE(bool(Physical)) << llvm::toString(Physical.takeError());
  auto Space = AddressSpace::create(*Physical, Page * 2);
  ASSERT_TRUE(bool(Space)) << llvm::toString(Space.takeError());
  const uint64_t Base = 0x100000;
  ASSERT_FALSE(bool((*Space)->map(Base, Page, Read | Write | UserAccessible)));
  const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
  ASSERT_FALSE(bool((*Space)->write(Base, Path)));
  ProcessOptions Options;
  Options.DarwinFiles.emplace();
  Options.DarwinFiles->Files["/data"] = Bytes;
  Options.MemoryLimit = Page * 2;
  Options.StackSize = Page;
  DarwinFiles Files(**Space, Options.DarwinFiles);
  DarwinMemory Memory(**Space, {Page, Base, {}}, Options);
  ProcessResult Result{ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       "native mapping comparison"};
  auto Opened =
      Files.handle(ServiceKind::Open, {0, 5, {Base, 0}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Opened)) << llvm::toString(Opened.takeError());
  ASSERT_TRUE(Opened->has_value());
  ASSERT_FALSE((**Opened).Error);
  const auto FD = (**Opened).Value;
  auto Mapped = Memory.handle(
      ServiceKind::Mmap, {0, 197, {0, 19, 3, 0x40002, FD, Page}, std::nullopt},
      Files, Result);
  ASSERT_TRUE(bool(Mapped)) << llvm::toString(Mapped.takeError());
  ASSERT_TRUE(Mapped->has_value()) << Result.Diagnostic;
  ASSERT_FALSE((**Mapped).Error);
  auto Closed =
      Files.handle(ServiceKind::Close, {0, 6, {FD}, std::nullopt}, Result);
  ASSERT_TRUE(bool(Closed)) << llvm::toString(Closed.takeError());
  ASSERT_TRUE(Closed->has_value());
  ASSERT_FALSE((**Closed).Error);
  std::vector<uint8_t> Observed(Page);
  ASSERT_FALSE(bool((*Space)->read((**Mapped).Value, Observed)));
  EXPECT_EQ(
      llvm::ArrayRef<uint8_t>(Observed),
      llvm::ArrayRef<uint8_t>(static_cast<const uint8_t *>(Native), Page));
  const auto Child = ::fork();
  ASSERT_GE(Child, 0);
  if (!Child) {
    const struct rlimit NoCore{0, 0};
    ::setrlimit(RLIMIT_CORE, &NoCore);
    const volatile auto Beyond = static_cast<volatile uint8_t *>(Native)[Page];
    (void)Beyond;
    ::_exit(0);
  }
  int Status = 0;
  ASSERT_EQ(::waitpid(Child, &Status, 0), Child);
  ASSERT_TRUE(WIFSIGNALED(Status));
  EXPECT_EQ(WTERMSIG(Status), SIGBUS);
#endif
}
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
  for (bool Directory : {false, true}) {
    SCOPED_TRACE(Directory);
    Native = {};
    ASSERT_EQ(::stat(Directory ? Root.c_str() : Input.c_str(), &Native), 0);
    std::optional<DarwinFileOptions> Options(std::in_place);
    if (Directory)
      Options->Directories.insert("/data");
    else
      Options->Files["/data"] = {'0', '1', '2', '3', '4',
                                 '5', '6', '7', '8', '9'};
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
    ASSERT_FALSE(
        bool((*Space)->map(Base, 4096, Read | Write | UserAccessible)));
    const uint8_t Path[] = {'/', 'd', 'a', 't', 'a', 0};
    ASSERT_FALSE(bool((*Space)->write(Base, Path)));
    DarwinFiles Files(**Space, Options);
    ProcessResult Result{
        ProcessProfile::MacOSMachO64, GuestArchitecture::AArch64,
        ExecutionBackendKind::Unicorn, "native ABI comparison"};
    auto Returned =
        Files.handle(ServiceKind::Stat64,
                     {0, 338, {Base, Base + 256}, std::nullopt}, Result);
    ASSERT_TRUE(bool(Returned)) << llvm::toString(Returned.takeError());
    ASSERT_TRUE(Returned->has_value()) << Result.Diagnostic;
    ASSERT_FALSE((**Returned).Error);
    std::array<uint8_t, 144> Bytes;
    ASSERT_FALSE(bool((*Space)->read(Base + 256, Bytes)));
    EXPECT_EQ(llvm::ArrayRef<uint8_t>(Bytes),
              llvm::ArrayRef<uint8_t>(
                  reinterpret_cast<const uint8_t *>(&Native), sizeof(Native)));
  }
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
  ASSERT_TRUE(std::filesystem::create_directory(Root / "empty"));
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

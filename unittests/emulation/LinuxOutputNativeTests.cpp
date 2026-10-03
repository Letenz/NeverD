//===- LinuxOutputNativeTests.cpp - Original workloads on host Linux -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <optional>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace neverd::emulation {
namespace {
#define NEVERD_LINUX_OUTPUT_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LINUX_OUTPUT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_TEXT
#undef NEVERD_LINUX_OUTPUT_VALUE

TEST(LinuxOutputNative, OriginalVectoredCallsMatchTheHostKernel) {
#if !defined(__linux__) || !defined(NEVERD_PROCESS_FIXTURE_DIR) ||             \
    (!defined(__x86_64__) && !defined(__aarch64__))
  GTEST_SKIP() << NativeUnavailable;
#else
  if (sysconf(_SC_PAGESIZE) != PageSize)
    GTEST_SKIP() << NativePageSize;
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory(TemporaryPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
#if defined(__x86_64__)
  const auto File = NativeX64;
#else
  const auto File = NativeARM64;
#endif
  const std::string Program =
      (std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) / File).string();
  struct TestCase {
    char Mode;
    llvm::StringRef Output, Error;
  };
  const TestCase Cases[] = {
#define NEVERD_LINUX_OUTPUT_CASE(Name, Mode, Out, Err)                         \
  {Mode, llvm::StringRef(Out, sizeof(Out) - 1),                                \
   llvm::StringRef(Err, sizeof(Err) - 1)},
#include "fixtures/LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_CASE
  };
  for (const auto &Test : Cases) {
    SCOPED_TRACE(Test.Mode);
    const std::string Mode(1, Test.Mode);
    const auto Out = (Root / OutputFile).string();
    const auto Err = (Root / ErrorFile).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
    std::string LaunchError;
    bool ExecutionFailed = false;
    const auto Status = llvm::sys::ExecuteAndWait(
        Program, {ExecutableName, Mode}, std::nullopt, Redirects, NativeTimeout,
        0, &LaunchError, &ExecutionFailed);
    ASSERT_FALSE(ExecutionFailed) << LaunchError;
    auto Output = llvm::MemoryBuffer::getFile(Out);
    auto Error = llvm::MemoryBuffer::getFile(Err);
    ASSERT_TRUE(bool(Output));
    ASSERT_TRUE(bool(Error));
    EXPECT_EQ(Status, ExitStatus) << LaunchError;
    EXPECT_EQ((*Output)->getBuffer(), Test.Output);
    EXPECT_EQ((*Error)->getBuffer(), Test.Error);
  }
#endif
}
} // namespace
} // namespace neverd::emulation

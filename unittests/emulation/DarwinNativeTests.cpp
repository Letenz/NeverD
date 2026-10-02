//===- DarwinNativeTests.cpp - Original workloads on the host kernel -----===//
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

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace neverd::emulation {
namespace {
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
  for (const char *Mode : {"return", "exit", "memory", "write-length"}) {
    SCOPED_TRACE(Mode);
    const auto Output = (Root / "stdout").string();
    const auto Error = (Root / "stderr").string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string LaunchError;
    bool ExecutionFailed = false;
    const auto Status = llvm::sys::ExecuteAndWait(
        Program, {Program, Mode}, std::nullopt, Redirects, 5, 0, &LaunchError,
        &ExecutionFailed);
    ASSERT_FALSE(ExecutionFailed) << LaunchError;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    ASSERT_TRUE(bool(Out));
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Err));
    EXPECT_EQ(Status, 37) << LaunchError << (*Err)->getBuffer().str();
    EXPECT_TRUE((*Err)->getBuffer().empty());
    const llvm::StringRef Name(Mode);
    EXPECT_EQ((*Out)->getBuffer(), Name == "memory"         ? "d"
                                   : Name == "write-length" ? "w"
                                                            : "");
  }
#endif
}
} // namespace
} // namespace neverd::emulation

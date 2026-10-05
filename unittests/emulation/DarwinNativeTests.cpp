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
#include <fstream>
#include <iterator>
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
    const auto Output = (Root / "stdout").string();
    const auto Error = (Root / "stderr").string();
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

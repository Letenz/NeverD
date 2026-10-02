//===- WindowsLifetimeTests.cpp - Native DLL lifecycle observations
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
#define NEVERD_LIFETIME_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_LIFETIME_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsLifetimeCases.def"
#undef NEVERD_LIFETIME_TEXT
#undef NEVERD_LIFETIME_VALUE
struct Case {
  const char *Name, *Argument;
  uint32_t Status;
};
constexpr Case Cases[] = {
#define NEVERD_LIFETIME_CASE(Name, Argument, Status) {#Name, Argument, Status},
#include "fixtures/WindowsLifetimeCases.def"
#undef NEVERD_LIFETIME_CASE
};
TEST(WindowsModuleLifetime, NativeWindowsObservesStartupAndTermination) {
#if !defined(NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR)
  GTEST_SKIP() << MissingTools;
#elif !defined(_WIN32) || !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  struct Cleanup {
    const std::filesystem::path &Path;
    ~Cleanup() { std::filesystem::remove_all(Path); }
  } Remove{Root};
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_LIFETIME_FIXTURE_DIR) / X64Dir;
  for (const char *File : {ProgramFile, LeafFile, MiddleFile})
    ASSERT_FALSE(llvm::sys::fs::copy_file((Directory / File).string(),
                                          (Root / File).string()));
  llvm::sys::Process::PreventCoreFiles();
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Name);
    const auto Program = (Root / ProgramFile).string();
    const auto Output = (Root / StdoutFile).string();
    const auto Error = (Root / StderrFile).string();
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                        Error};
    std::string Diagnostic;
    bool Failed = false;
    const int Status = llvm::sys::ExecuteAndWait(
        Program, {Program, C.Argument}, std::nullopt, Redirects,
        NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
    EXPECT_FALSE(Failed) << Diagnostic;
    auto Out = llvm::MemoryBuffer::getFile(Output);
    auto Err = llvm::MemoryBuffer::getFile(Error);
    ASSERT_TRUE(bool(Out));
    ASSERT_TRUE(bool(Err));
    llvm::outs() << C.Name << ": " << llvm::toHex((*Out)->getBuffer()) << "\n";
    EXPECT_EQ(uint32_t(Status), C.Status) << llvm::toHex((*Err)->getBuffer());
    EXPECT_FALSE((*Out)->getBuffer().empty());
    EXPECT_TRUE((*Err)->getBuffer().empty());
  }
#endif
}
} // namespace
} // namespace neverd::emulation

//===- WindowsDynamicTests.cpp - Original runtime loader observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>

namespace neverd::emulation {
namespace {
#define NEVERD_DYNAMIC_VALUE(Name, Value) constexpr uint64_t Name = Value;
#define NEVERD_DYNAMIC_TEXT(Name, Text) constexpr char Name[] = Text;
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_TEXT
#undef NEVERD_DYNAMIC_VALUE
struct Case {
  const char *Name, *Argument;
};
constexpr Case Cases[] = {
#define NEVERD_DYNAMIC_CASE(Name, Argument) {#Name, Argument},
#include "fixtures/WindowsDynamicCases.def"
#undef NEVERD_DYNAMIC_CASE
};
TEST(WindowsDynamicOracle, NativeWindowsLoadsAndUnloadsOriginalImages) {
#if !defined(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) || !defined(_WIN32) ||        \
    !defined(_M_X64)
  GTEST_SKIP() << NativeOnly;
#else
  const auto Directory =
      std::filesystem::path(NEVERD_WINDOWS_DYNAMIC_FIXTURE_DIR) / X64Dir;
  llvm::SmallString<128> Temporary;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(TempPrefix, Temporary));
  const std::filesystem::path Root(Temporary.str().str());
  auto Cleanup = llvm::scope_exit([&] { std::filesystem::remove_all(Root); });
  for (bool NoEntry : {false, true}) {
    for (const char *File :
         {ProgramFile, StaticProgramFile, LeafFile, MiddleFile, TopFile})
      ASSERT_FALSE(llvm::sys::fs::copy_file(
          ((NoEntry && (File == LeafFile || File == MiddleFile)
                ? Directory / NoEntryDirectory
                : Directory) /
           File)
              .string(),
          (Root / File).string()));
    for (const char *File : {ProgramFile, StaticProgramFile}) {
      const auto Program = (Root / File).string();
      const auto Output = (Root / StdoutFile).string(),
                 Error = (Root / StderrFile).string();
      const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Output,
                                                          Error};
      for (const auto &C : Cases) {
        if (File == StaticProgramFile && &C != &Cases[0])
          continue;
        if (NoEntry &&
            (C.Argument[1] == FailedMiddleMode || C.Argument[1] == LeafRole))
          continue;
        SCOPED_TRACE(C.Name);
        std::string Diagnostic;
        bool Failed = false;
        const int Status = llvm::sys::ExecuteAndWait(
            Program, {Program, C.Argument}, std::nullopt, Redirects,
            NativeTimeoutSeconds, 0, &Diagnostic, &Failed);
        ASSERT_FALSE(Failed) << Diagnostic;
        auto Out = llvm::MemoryBuffer::getFile(Output),
             Err = llvm::MemoryBuffer::getFile(Error);
        ASSERT_TRUE(bool(Out));
        ASSERT_TRUE(bool(Err));
        llvm::outs() << ObservationLabel << NoEntry << ' ' << File << ' '
                     << C.Argument << ' ' << Status << ' '
                     << llvm::toHex((*Out)->getBuffer()) << '\n';
        EXPECT_EQ(Status, ExitStatus) << llvm::toHex((*Err)->getBuffer());
        EXPECT_TRUE((*Err)->getBuffer().empty());
      }
    }
  }
#endif
}
} // namespace
} // namespace neverd::emulation

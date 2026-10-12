//===- LLVMCLongInitializerTests.cpp - Initializers over several lines ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

#include <memory>
#include <string>
#include <vector>

namespace {
// As many bytes and words as an image section mirror holds in a few pages.
constexpr unsigned Entries = 6000;

/// Every escape the printer spells -- NUL, newline, tab, quote, backslash,
/// question mark and bytes past ASCII -- and a last byte that is no NUL,
/// which the printer would drop.
std::string bytes() {
  std::string Bytes;
  for (unsigned I = 0; I < Entries; ++I)
    Bytes += static_cast<char>((I * 37 + 11) & 0xFF);
  Bytes.back() = 'z';
  return Bytes;
}

std::string module() {
  std::string IR =
      "@text = constant [" + std::to_string(Entries) + " x i8] c\"";
  for (unsigned char Byte : bytes())
    IR += "\\" + llvm::utohexstr(Byte, /*LowerCase=*/false, /*Width=*/2);
  IR += "\"\n@table = constant [" + std::to_string(Entries) + " x i32] [";
  for (unsigned I = 0; I < Entries; ++I)
    IR += (I ? ", i32 " : "i32 ") + std::to_string(I * 2654435761u);
  IR += "]\n";
  IR += R"(
define i32 @exercise(i32 %i) {
  %x = zext i32 %i to i64
  %w = getelementptr inbounds [)" +
        std::to_string(Entries) + R"( x i32], ptr @table, i64 0, i64 %x
  %v = load i32, ptr %w
  %b = getelementptr inbounds [)" +
        std::to_string(Entries) + R"( x i8], ptr @text, i64 0, i64 %x
  %c = load i8, ptr %b
  %z = zext i8 %c to i32
  %r = add i32 %v, %z
  ret i32 %r
}
)";
  return IR;
}

/// Build \p C with a harness that reads every entry back, and run it.
void expectEveryEntry(const std::string &C) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Found = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Found));
  const std::string Compiler = *Found;
#endif
  const std::string Harness = R"(
#include <stdint.h>
uint32_t exercise(uint32_t);
int main(void) {
  for (uint32_t i = 0; i < )" +
                              std::to_string(Entries) +
                              R"(u; ++i) {
    const uint32_t byte = i == )" +
                              std::to_string(Entries - 1) +
                              R"(u ? 'z' : ((i * 37u + 11u) & 0xFFu);
    if (exercise(i) != i * 2654435761u + byte)
      return 1;
  }
  return 0;
}
)";
  llvm::SmallString<128> Paths[4];
  std::vector<std::unique_ptr<llvm::FileRemover>> Cleanup;
  const char *Suffixes[] = {"c", "c", "exe", "err"};
  for (unsigned I = 0; I < 4; ++I) {
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-long-initializer",
                                                    Suffixes[I], Paths[I]));
    Cleanup.push_back(std::make_unique<llvm::FileRemover>(Paths[I]));
  }
  const std::string *Contents[] = {&C, &Harness};
  for (unsigned I = 0; I < 2; ++I) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Paths[I], Error);
    ASSERT_FALSE(Error);
    Out << *Contents[I];
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, Paths[3].str()};
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    llvm::SmallVector<llvm::StringRef, 8> Args{
        Compiler, "-std=c11", Level, Paths[0], Paths[1], "-o", Paths[2]};
    std::string Error;
    const int Built = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                                Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(Paths[3]);
    ASSERT_EQ(Built, 0) << Error
                        << (Errors ? (*Errors)->getBuffer().str() : "");
    const int Ran = llvm::sys::ExecuteAndWait(
        Paths[2], {Paths[2]}, std::nullopt, Redirects, 60, 0, &Error);
    EXPECT_EQ(Ran, 0) << Error;
  }
}
} // namespace

// A section mirror's initializer printed on one line ran to megabytes, past
// what a source page holds.  A long one prints over lines of bounded width,
// a string as adjacent literals, and keeps every byte and word.
TEST(LLVMCLongInitializers, PrintOverBoundedLinesAndKeepTheirValues) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto M = llvm::parseAssemblyString(module(), Error, Context);
  ASSERT_TRUE(M) << Error.getMessage().str();
  ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string C;
  llvm::raw_string_ostream Out(C);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*M, Out, Options));

  size_t Widest = 0, Lines = 0;
  for (llvm::StringRef Rest = C; !Rest.empty();) {
    const auto [Line, Next] = Rest.split('\n');
    Widest = std::max(Widest, Line.size());
    ++Lines;
    Rest = Next;
  }
  EXPECT_LE(Widest, 200u) << C;
  // Each initializer took many lines.
  EXPECT_GT(Lines, 2 * Entries * 4 / 120);
  expectEveryEntry(C);
}

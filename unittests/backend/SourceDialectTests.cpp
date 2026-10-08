//===- SourceDialectTests.cpp - Emitted C spelled as Rust and Go ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CSyntax.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/dialect/SourceDialect.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <cstdlib>
#include <map>

using namespace neverd;

namespace {

SourceDialectText spell(llvm::StringRef C, SourceDialect Dialect,
                        std::vector<CSourceName> Names = {}) {
  SourceDialectOptions Opts;
  Opts.Dialect = Dialect;
  Opts.Names = Names;
  return spellInDialect(C, Opts);
}

/// The significant tokens of a C text, for comparing a text printed back.
std::vector<std::string> tokens(llvm::StringRef C) {
  std::vector<std::string> Result;
  auto Lexed = csyntax::lex(C);
  if (!Lexed) {
    llvm::consumeError(Lexed.takeError());
    return Result;
  }
  for (const csyntax::Token &T : *Lexed)
    if (T.Kind != csyntax::TokenKind::Comment &&
        T.Kind != csyntax::TokenKind::End)
      Result.push_back(T.Text.str());
  return Result;
}

TEST(SourceDialect, ConvertsWhatCPromotes) {
  const char *C = "#include <stdint.h>\n"
                  "uint32_t f(uint8_t a, uint8_t b, int32_t s, uint32_t u) {\n"
                  "    uint32_t x;\n"
                  "    x = a + b;\n"
                  "    if (s < u) {\n"
                  "        return (uint8_t)(a + b);\n"
                  "    }\n"
                  "    return x;\n"
                  "}\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("unsafe fn f(a: u8, b: u8, s: i32, u: u32) -> u32 {"),
            std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("x = a as u32 + b as u32;"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("if (s as u32) < u {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("return (a + b) as u32;"), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("func f(a uint8, b uint8, s int32, u uint32) uint32 {"),
            std::string::npos)
      << Go;
  EXPECT_NE(Go.find("x = uint32(a) + uint32(b)"), std::string::npos) << Go;
  EXPECT_NE(Go.find("if uint32(s) < u {"), std::string::npos) << Go;
  EXPECT_NE(Go.find("return uint32(a + b)"), std::string::npos) << Go;
}

TEST(SourceDialect, SpellsMemoryAndTruthiness) {
  const char *C = "#include <stdint.h>\n"
                  "void g(void *arg0, int64_t v) {\n"
                  "    uint64_t t;\n"
                  "    *(uint64_t *)(arg0 + 8) = 0;\n"
                  "    t = *(uint64_t *)(uintptr_t)(v + 16);\n"
                  "    if (v) {\n"
                  "        t = (__builtin_trap(), 0 /* unknown value */);\n"
                  "    }\n"
                  "    if (arg0) {\n"
                  "        return;\n"
                  "    }\n"
                  "}\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("*(arg0.byte_add(8) as *mut u64) = 0;"),
            std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("t = *((v + 16) as *mut u64);"), std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("if v != 0 {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("t = abort() /* unknown value */;"), std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("if !arg0.is_null() {"), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("*(*uint64)(unsafe.Add(arg0, 8)) = 0"), std::string::npos)
      << Go;
  EXPECT_NE(Go.find("t = *(*uint64)(v + 16)"), std::string::npos) << Go;
  EXPECT_NE(Go.find("t = trap() /* unknown value */"), std::string::npos) << Go;
  EXPECT_NE(Go.find("if arg0 != nil {"), std::string::npos) << Go;
}

TEST(SourceDialect, NamesSymbolsAsTheirLanguage) {
  const char *C = "#include <stdint.h>\n"
                  "extern int64_t core_fmt_write() "
                  "__asm__(\"_ZN4core3fmt5write17h0123456789abcdefE\");\n"
                  "/* main.main */\n"
                  "int64_t main_main(void) {\n"
                  "    return core_fmt_write(1);\n"
                  "}\n";
  std::vector<CSourceName> Names = {
      {CSourceName::Kind::Function, "core_fmt_write",
       "_ZN4core3fmt5write17h0123456789abcdefE", std::nullopt},
      {CSourceName::Kind::Function, "main_main", "main.main", 0x401000}};
  const std::string Rust = spell(C, SourceDialect::Rust, Names).Text;
  EXPECT_NE(Rust.find("return core::fmt::write(1);"), std::string::npos)
      << Rust;
  EXPECT_NE(Rust.find("unsafe fn main_main() -> i64 {"), std::string::npos)
      << Rust;
  const std::string Go = spell(C, SourceDialect::Go, Names).Text;
  EXPECT_NE(Go.find("func main.main() int64 {"), std::string::npos) << Go;
  // The comment that only repeated the name is gone.
  EXPECT_EQ(Go.find("/* main.main */"), std::string::npos) << Go;
}

TEST(SourceDialect, KeepsCBreakAndContinue) {
  const char *C = "#include <stdint.h>\n"
                  "int32_t h(int32_t v, int32_t n) {\n"
                  "    while (1) {\n"
                  "        switch (v) {\n"
                  "        case 1:\n"
                  "        case 2:\n"
                  "            if (n) {\n"
                  "                break;\n"
                  "            }\n"
                  "            n = 0;\n"
                  "            break;\n"
                  "        default:\n"
                  "            return n;\n"
                  "        }\n"
                  "        do {\n"
                  "            if (n == 3) {\n"
                  "                continue;\n"
                  "            }\n"
                  "            n = n - 1;\n"
                  "        } while (n > 0);\n"
                  "    }\n"
                  "}\n";
  const std::string Rust = spell(C, SourceDialect::Rust).Text;
  EXPECT_NE(Rust.find("'switch1: {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("1 | 2 => {"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("break 'switch1;"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("break 'body2;"), std::string::npos) << Rust;
  EXPECT_NE(Rust.find("if !(n > 0) { break; }"), std::string::npos) << Rust;
  const std::string Go = spell(C, SourceDialect::Go).Text;
  EXPECT_NE(Go.find("case 1, 2:"), std::string::npos) << Go;
  EXPECT_NE(Go.find("for again := true; again; again = n > 0 {"),
            std::string::npos)
      << Go;
}

TEST(SourceDialect, ShowsWhatItCannotSpellAsC) {
  const char *C = "#include <stdint.h>\n"
                  "int64_t k(int64_t v) {\n"
                  "    return v++ + 1;\n"
                  "}\n"
                  "int64_t m(int64_t v) {\n"
                  "    return v;\n"
                  "}\n";
  SourceDialectText Rust = spell(C, SourceDialect::Rust);
  ASSERT_EQ(Rust.Unread.size(), 1u) << Rust.Text;
  EXPECT_NE(Rust.Unread.front().find("increment"), std::string::npos);
  EXPECT_NE(Rust.Text.find("// NeverD: shown as C: an increment"),
            std::string::npos);
  EXPECT_NE(Rust.Text.find("    return v++ + 1;"), std::string::npos);
  EXPECT_NE(Rust.Text.find("unsafe fn m(v: i64) -> i64 {"), std::string::npos)
      << Rust.Text;
}

/// NEVERD_SOURCE_DIALECT_CORPUS names a directory of emitted C files: every
/// one must read back as the same C tokens, and the reasons for anything
/// shown as C are counted.
TEST(SourceDialect, CorpusReadsBackAsTheSameC) {
  const char *Dir = std::getenv("NEVERD_SOURCE_DIALECT_CORPUS");
  if (!Dir)
    GTEST_SKIP() << "NEVERD_SOURCE_DIALECT_CORPUS is not set";
  std::error_code EC;
  std::map<std::string, unsigned> Reasons;
  std::map<std::string, std::string> Examples;
  unsigned Files = 0, Mismatches = 0;
  size_t CBytes = 0, RustBytes = 0, GoBytes = 0;
  for (llvm::sys::fs::recursive_directory_iterator It(Dir, EC), End;
       It != End && !EC; It.increment(EC)) {
    if (!llvm::StringRef(It->path()).ends_with(".c"))
      continue;
    auto Buffer = llvm::MemoryBuffer::getFile(It->path());
    ASSERT_TRUE(Buffer) << It->path();
    const llvm::StringRef C = (*Buffer)->getBuffer();
    ++Files;
    CBytes += C.size();
    SourceDialectText Back = spell(C, SourceDialect::C);
    for (const std::string &Reason : Back.Unread) {
      // Group by the reason's words, not its offsets.
      const std::string Key =
          llvm::StringRef(Reason).split(" (byte").first.str();
      if (!Reasons[Key]++)
        Examples[Key] = It->path() + ": " + Reason;
    }
    if (Back.Unread.empty()) {
      const std::vector<std::string> Original = tokens(C);
      const std::vector<std::string> Printed = tokens(Back.Text);
      if (Printed != Original) {
        ++Mismatches;
        size_t I = 0;
        while (I < Original.size() && I < Printed.size() &&
               Original[I] == Printed[I])
          ++I;
        std::string Was, Now;
        for (size_t J = I >= 8 ? I - 8 : 0; J < I + 8; ++J) {
          if (J < Original.size())
            Was += Original[J] + " ";
          if (J < Printed.size())
            Now += Printed[J] + " ";
        }
        ADD_FAILURE() << It->path() << " reads back differently at token " << I
                      << ":\n  C:    " << Was << "\n  back: " << Now;
      }
    }
    for (SourceDialect D : {SourceDialect::Rust, SourceDialect::Go}) {
      SourceDialectText Spelled = spell(C, D);
      (D == SourceDialect::Rust ? RustBytes : GoBytes) += Spelled.Text.size();
      for (const std::string &Reason : Spelled.Unread) {
        const std::string Key = (sourceDialectKey(D) + ": " +
                                 llvm::StringRef(Reason).split(" (byte").first)
                                    .str();
        if (!Reasons[Key]++)
          Examples[Key] = It->path() + ": " + Reason;
      }
    }
  }
  std::vector<std::pair<unsigned, std::string>> Sorted;
  for (auto &[Reason, Count] : Reasons)
    Sorted.push_back({Count, Reason});
  std::sort(Sorted.rbegin(), Sorted.rend());
  std::string Report;
  for (auto &[Count, Reason] : Sorted)
    Report += "  " + std::to_string(Count) + "  " + Reason + "\n      " +
              Examples[Reason] + "\n";
  std::printf("%u files, %zu C bytes, %zu Rust bytes, %zu Go bytes, %u "
              "mismatches\nshown as C:\n%s",
              Files, CBytes, RustBytes, GoBytes, Mismatches, Report.c_str());
}

/// NEVERD_SOURCE_DIALECT_FILE and NEVERD_SOURCE_DIALECT (`c`, `rust` or
/// `go`) print one file in a dialect, for looking at the output.
TEST(SourceDialect, OneFile) {
  const char *File = std::getenv("NEVERD_SOURCE_DIALECT_FILE");
  const char *Key = std::getenv("NEVERD_SOURCE_DIALECT");
  if (!File || !Key)
    GTEST_SKIP() << "NEVERD_SOURCE_DIALECT_FILE is not set";
  auto Buffer = llvm::MemoryBuffer::getFile(File);
  ASSERT_TRUE(Buffer) << File;
  auto Dialect = sourceDialectFromKey(Key);
  ASSERT_TRUE(Dialect) << Key;
  SourceDialectText Spelled = spell((*Buffer)->getBuffer(), *Dialect);
  std::printf("=====BEGIN\n%s=====END\n", Spelled.Text.c_str());
  for (const std::string &Reason : Spelled.Unread)
    std::printf("shown as C: %s\n", Reason.c_str());
}

} // namespace

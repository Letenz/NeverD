//===- CSymbolNameTests.cpp - The C names emitted for symbols -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
#endif

using namespace neverd;

namespace {

constexpr auto NotFound = std::string::npos;

HighStmt callStatement(const std::string &Target, va_t Address,
                       std::vector<ExprPtr> Arguments = {}) {
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall(Target, Address, std::move(Arguments));
  return Call;
}

HighStmt returnZero() {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 8);
  return Return;
}

HighFunc function(const char *Name, va_t Entry, std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = Entry;
  F.ReturnType = NdType::makeInt(8, false);
  F.Body = std::move(Body);
  F.Body.push_back(returnZero());
  return F;
}

std::string emitHighC(const std::vector<HighFunc> &Funcs, BinaryFormat Format,
                      Arch Target) {
  CEmitterOptions Options;
  Options.TheArch = Target;
  Options.Format = Format;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
  return Text;
}

/// Foo::run() calls two constructors of Bar, operator delete and a C
/// function whose own name starts with underscores.
std::vector<HighFunc> cxxCaller(const char *Prefix) {
  const std::string P = Prefix;
  return {function(
      (P + "_ZN3Foo3runEv").c_str(), 0x1000,
      {callStatement(P + "_ZN3BarC1Ev", 0x2000),
       callStatement(P + "_ZN3BarC2Ei", 0x2010, {HighExpr::makeConst(7, 4)}),
       callStatement(P + "_ZdlPv", 0x2020, {HighExpr::makeConst(0, 8)}),
       callStatement(P + "__nd_probe_hook", 0x2030)})};
}

TEST(CSymbolNames, ABIUnderscoreOnlyWhereTheFormatAddsOne) {
  EXPECT_TRUE(hasABIUnderscore(BinaryFormat::MachO, Arch::AArch64));
  EXPECT_TRUE(hasABIUnderscore(BinaryFormat::COFF, Arch::X86));
  EXPECT_FALSE(hasABIUnderscore(BinaryFormat::COFF, Arch::X64));
  EXPECT_FALSE(hasABIUnderscore(BinaryFormat::ELF, Arch::X64));
  EXPECT_EQ(cNameOfSymbol("_printf", BinaryFormat::MachO, Arch::X64), "printf");
  EXPECT_EQ(cNameOfSymbol("__libc_start_main", BinaryFormat::ELF, Arch::X64),
            "__libc_start_main");
  EXPECT_EQ(symbolOfCName("_ZdlPv", BinaryFormat::MachO, Arch::AArch64),
            "__ZdlPv");
  EXPECT_EQ(symbolOfCName("_ZdlPv", BinaryFormat::ELF, Arch::X64), "_ZdlPv");
}

TEST(CSymbolNames, ElfCxxCallsReadByScopeAndLinkByMangledName) {
  const std::string Source =
      emitHighC(cxxCaller(""), BinaryFormat::ELF, Arch::X64);
  // The definition reads by its scopes and names its signature above.
  EXPECT_NE(Source.find("/* Foo::run() */\n"), NotFound) << Source;
  EXPECT_NE(Source.find(" Foo_run("), NotFound) << Source;
  // Both constructors stay distinct, each linking by its own symbol.
  EXPECT_NE(
      Source.find("Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */"),
      NotFound)
      << Source;
  EXPECT_NE(
      Source.find("Bar_ctor_2() __asm__(\"_ZN3BarC2Ei\"); /* Bar::Bar(int) */"),
      NotFound)
      << Source;
  EXPECT_NE(Source.find("operator_delete() __asm__(\"_ZdlPv\"); /* operator "
                        "delete(void*) */"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("Bar_ctor();"), NotFound) << Source;
  EXPECT_NE(Source.find("Bar_ctor_2(7);"), NotFound) << Source;
  EXPECT_NE(Source.find("operator_delete(0);"), NotFound) << Source;
  // An ELF symbol's underscores are part of its name.
  EXPECT_NE(Source.find("extern int __nd_probe_hook();"), NotFound) << Source;
  EXPECT_NE(Source.find("__nd_probe_hook();"), NotFound) << Source;
  // MSVC's member rules do not describe Itanium constructors.
  EXPECT_EQ(Source.find("__fastcall"), NotFound) << Source;
}

TEST(CSymbolNames, RenamedNoreturnImportsStillNeverReturn) {
  // std::__throw_bad_alloc() reads by its scopes, but its call still ends the
  // path; the declaration must say so, or C falls through.
  const std::string Source =
      emitHighC({function("_ZN3Foo3runEv", 0x1000,
                          {callStatement("_ZSt17__throw_bad_allocv", 0x2000)})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find(" std___throw_bad_alloc() "
                        "__asm__(\"_ZSt17__throw_bad_allocv\") "
                        "__attribute__((noreturn));"),
            NotFound)
      << Source;
}

TEST(CSymbolNames, KnownAritiesFollowTheSymbol) {
  // The runtime tables know `_Unwind_Resume` and `_ZSt9terminatev` by their
  // symbols, which their C names keep or read by scope; declarations and
  // calls take the tables' arity either way.
  const std::string Source =
      emitHighC({function("resume", 0x1000,
                          {callStatement("_Unwind_Resume", 0x2000,
                                         {HighExpr::makeConst(1, 8),
                                          HighExpr::makeConst(2, 8)})}),
                 function("terminate", 0x1100,
                          {callStatement("_ZSt9terminatev", 0x2010,
                                         {HighExpr::makeConst(3, 8)})})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern int _Unwind_Resume(int64_t)"), NotFound)
      << Source;
  EXPECT_NE(Source.find("_Unwind_Resume(1);"), NotFound) << Source;
  EXPECT_NE(Source.find(
                "extern int std_terminate(void) __asm__(\"_ZSt9terminatev\")"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("std_terminate();"), NotFound) << Source;
}

TEST(CSymbolNames, KnownArgumentsFillOneRegisterEach) {
  // MSVC's ARM compiler calls __intrinsic_setjmpex with the buffer and the
  // frame, and longjmp takes the buffer and the value everywhere: the
  // registers after them are not arguments, and on a 32-bit target each
  // argument is one 32-bit register.
  const std::string Source = emitHighC(
      {function(
          "jumps", 0x1000,
          {callStatement("__intrinsic_setjmpex", 0x2000,
                         {HighExpr::makeConst(1, 4), HighExpr::makeConst(2, 4),
                          HighExpr::makeConst(3, 4)}),
           callStatement("longjmp", 0x2010,
                         {HighExpr::makeConst(1, 4), HighExpr::makeConst(4, 4),
                          HighExpr::makeConst(5, 4),
                          HighExpr::makeConst(6, 4)})})},
      BinaryFormat::COFF, Arch::ARM);
  EXPECT_NE(Source.find("extern int __intrinsic_setjmpex(int32_t, int32_t);"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("__intrinsic_setjmpex(1, 2);"), NotFound) << Source;
  EXPECT_NE(Source.find("longjmp(1, 4);"), NotFound) << Source;
}

TEST(CSymbolNames, UndeterminedKnownArgumentsAreUnknown) {
  // A catch fragment whose incoming exception object the lift does not
  // track still passes it: the call takes the declared argument, unknown.
  const std::string Source =
      emitHighC({function("catch_fragment", 0x1000,
                          {callStatement("__cxa_begin_catch", 0x2000)})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern int __cxa_begin_catch(int64_t);"), NotFound)
      << Source;
  EXPECT_NE(Source.find("__cxa_begin_catch((__builtin_trap(), 0 /* unknown "
                        "value */));"),
            NotFound)
      << Source;
}

TEST(CSymbolNames, MachOLabelsCarryTheDecoration) {
  const std::string Source =
      emitHighC(cxxCaller("_"), BinaryFormat::MachO, Arch::AArch64);
  EXPECT_NE(Source.find(" Foo_run("), NotFound) << Source;
  EXPECT_NE(Source.find("Bar_ctor() __asm__(\"__ZN3BarC1Ev\");"), NotFound)
      << Source;
  // The C name of `___nd_probe_hook` is `__nd_probe_hook`; the compiler adds
  // the decoration back, so it needs no label.
  EXPECT_NE(Source.find("extern int __nd_probe_hook();"), NotFound) << Source;
}

TEST(CSymbolNames, DefinitionsStepAsideFromTheCRuntime) {
  const std::string Source = emitHighC(
      {function("_start", 0x1000, {callStatement("_helper", 0x1100)}),
       function("_helper", 0x1100, {}), function("_init", 0x1200, {})},
      BinaryFormat::ELF, Arch::X64);
  // glibc's start files define _start and _init themselves.
  EXPECT_NE(Source.find(" start("), NotFound) << Source;
  EXPECT_NE(Source.find(" init("), NotFound) << Source;
  EXPECT_EQ(Source.find(" _start("), NotFound) << Source;
  // Other definitions keep their names.
  EXPECT_NE(Source.find(" _helper("), NotFound) << Source;
  EXPECT_NE(Source.find("_helper();"), NotFound) << Source;
}

TEST(CSymbolNames, LLVMCDeclaresCxxImportsByMangledName) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare i64 @_ZN3BarC1Ev(i64)
define i64 @_ZN3Foo3runEv(i64 %this) {
  %r = call i64 @_ZN3BarC1Ev(i64 %this)
  ret i64 %r
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_NE(
      Source.find(
          "Bar_ctor(uint64_t) __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */"),
      NotFound)
      << Source;
  EXPECT_NE(Source.find("/* Foo::run() */\n"), NotFound) << Source;
  EXPECT_NE(Source.find(" Foo_run("), NotFound) << Source;
}

TEST(CSymbolNames, ElfCxxLabelsLinkToTheMangledDefinitions) {
#if !defined(__linux__)
  GTEST_SKIP() << "links an ELF program on the host";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // The stubs define the symbols by their mangled names, as a C++ library
  // would; the emitted C must reach each one through its label.
  const std::string Stubs = R"(
#include <stdint.h>
unsigned called;
int64_t Foo_run(void);
void _ZN3BarC1Ev(void) { called |= 1; }
void _ZN3BarC2Ei(int value) { if (value == 7) called |= 2; }
void _ZdlPv(void *pointer) { if (!pointer) called |= 4; }
void __nd_probe_hook(void) { called |= 8; }
int main(void) { Foo_run(); return called == 15 ? 0 : 1; }
)";
  const std::string Source =
      emitHighC(cxxCaller(""), BinaryFormat::ELF, Arch::X64);
  llvm::SmallString<128> SourcePath, StubPath, BinaryPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-c-names", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-c-names-stubs", "c",
                                                  StubPath));
  llvm::FileRemover RemoveStubs(StubPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-c-names", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  for (const auto &[Path, Text] : {std::pair{SourcePath.str(), Source},
                                   std::pair{StubPath.str(), Stubs}}) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out << Text;
  }
  std::string Message;
  const int Compiled =
      llvm::sys::ExecuteAndWait(NEVERD_RUNTIME_FIXTURE_COMPILER,
                                {NEVERD_RUNTIME_FIXTURE_COMPILER, "-std=c11",
                                 "-w", SourcePath, StubPath, "-o", BinaryPath},
                                std::nullopt, {}, 60, 0, &Message);
  ASSERT_EQ(Compiled, 0) << Message << '\n' << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                      {}, 30, 0, &Message),
            0)
      << Message << '\n'
      << Source;
#endif
}

} // namespace

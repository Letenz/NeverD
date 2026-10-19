//===- X86_32_PICLibraryTests.cpp - i386 position-independent libraries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// An i386 shared library's PLT entry jumps through its GOT entry by the GOT
// address its caller holds in EBX (`jmp *disp(%ebx)`), not through an
// absolute slot.  The entry is the import whose slot that is.  Its functions
// find that address by calling a get-PC thunk, which loads one register with
// the return address and leaves every other one as it was.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <optional>
#include <regex>
#include <string>

using namespace neverd;

namespace {

/// The fixture \p File, the assembled object or the library linked from it.
std::optional<BinaryImage> loadFixture(llvm::StringRef File) {
  const std::filesystem::path Path =
      std::filesystem::path(TEST_OBJ_DIR) / File.str();
  if (!std::filesystem::exists(Path))
    return std::nullopt;
  auto ImageOrErr = loadBinary(Path);
  if (!ImageOrErr) {
    ADD_FAILURE() << llvm::toString(ImageOrErr.takeError());
    return std::nullopt;
  }
  return std::move(*ImageOrErr);
}

std::optional<BinaryImage> loadLibrary() {
  return loadFixture("test_pic_library_i386.so");
}

/// The HighC source of the library's function \p Name, lifted with the
/// functions \p With so that the contracts between them settle.
std::string decompile(const BinaryImage &Img, llvm::StringRef Name,
                      llvm::ArrayRef<llvm::StringRef> With = {}) {
  const Symbol *Function = Img.findSymbol(Name);
  if (!Function)
    return "no symbol " + Name.str();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Function->Addr};
  for (llvm::StringRef Other : With) {
    const Symbol *Each = Img.findSymbol(Other);
    if (!Each)
      return "no symbol " + Other.str();
    Opts.OnlyFunctionEntries.insert(Each->Addr);
  }
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success)
    return "pipeline failed: " + Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit(Result.HighFuncs, OS, Options))
    return "emission failed";
  return Source;
}

TEST(X86_32PICLibrary, PLTEntryThroughTheGOTIsTheImport) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Section *Plt = Img->getPltSection();
  ASSERT_NE(Plt, nullptr);
  // The entry after PLT0.
  const Import *Imp = Img->findImportStubAt(Plt->VA + 16);
  ASSERT_NE(Imp, nullptr);
  EXPECT_EQ(Imp->Name, "fputs");
  const std::string Source = decompile(*Img, "put");
  EXPECT_NE(Source.find("fputs("), std::string::npos) << Source;
}

// Taken for an ordinary call, a get-PC thunk call kept the old value of the
// register it loads and lost %ecx, which `scale` and `twice` keep across it,
// so both the GOT address and the sum came out unknown.  `twice` calls a
// thunk padded with nop.
TEST(X86_32PICLibrary, GetPcThunkCallLoadsOnlyItsRegister) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  for (const char *Name : {"put", "scale", "twice"}) {
    SCOPED_TRACE(Name);
    const std::string Source = decompile(*Img, Name);
    EXPECT_EQ(Source.find("get_pc_thunk"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
    if (llvm::StringRef(Name) != "put")
      EXPECT_NE(Source.find("factor"), std::string::npos) << Source;
  }
}

/// Expect neither of \p Img's call/pop functions to print the return address
/// its call pushed, and each to keep what it computes.
void expectNoGetPcPush(const BinaryImage &Img) {
  for (const char *Name : {"clang_scale", "clang_lower"}) {
    SCOPED_TRACE(Name);
    const Symbol *Pop = Img.findSymbol(std::string(Name) + "_pop");
    ASSERT_NE(Pop, nullptr);
    const std::string Source = decompile(Img, Name);
    // The writer spells the return address in uppercase hexadecimal.
    EXPECT_FALSE(std::regex_search(
        Source, std::regex("\\b0x" + llvm::utohexstr(Pop->Addr) + "\\b")))
        << Source;
    EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
  }
  const std::string Scale = decompile(Img, "clang_scale");
  EXPECT_NE(Scale.find("factor"), std::string::npos) << Scale;
  // The byte the call passes keeps its zero extension.
  const std::string Lower = decompile(Img, "clang_lower");
  EXPECT_NE(Lower.find("(uint8_t)arg0"), std::string::npos) << Lower;
}

// Clang's call to the next instruction pushes the return address for the pop
// alone, which reads it as the PC.  A frame realigned to 16 bytes kept that
// push as a local of the C (`var_m24 = 0x146E;`), whether the pop's value
// was read, as `clang_scale` reads `factor` through it, or not: `clang_lower`
// keeps it in %ebx only for its PLT call.
TEST(X86_32PICLibrary, GetPcCallPushIsNoLocal) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  expectNoGetPcPush(*Img);
}

// `clang_reload` spills the GOT address before its PLT call and reloads it to
// read `factor`.  Every write of the slot stores the GOT address, so the
// reload reads it and the read names `factor`, and the spill is left out.
// Before, the C stored the GOT address and read `*(int32_t *)(slot + N)`.
TEST(X86_32PICLibrary, ReloadedGOTAddressNamesItsData) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *GOT = Img->findSymbol("_GLOBAL_OFFSET_TABLE_");
  ASSERT_NE(GOT, nullptr);
  const std::string Source = decompile(*Img, "clang_reload");
  EXPECT_NE(Source.find("factor"), std::string::npos) << Source;
  EXPECT_NE(Source.find("tolower("), std::string::npos) << Source;
  EXPECT_FALSE(std::regex_search(
      Source, std::regex("\\b0x" + llvm::utohexstr(GOT->Addr) + "\\b")))
      << Source;
  EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
}

// `clang_set_factor` returns with the GOT address its call/pop left in %eax,
// which addresses its data and is no value it means: the C returns nothing,
// where it returned the GOT address as an int.
TEST(X86_32PICLibrary, AGOTAddressLeftInTheReturnRegisterIsNoValue) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *GOT = Img->findSymbol("_GLOBAL_OFFSET_TABLE_");
  ASSERT_NE(GOT, nullptr);
  const std::string Source = decompile(*Img, "clang_set_factor");
  EXPECT_NE(Source.find("void clang_set_factor("), std::string::npos) << Source;
  EXPECT_NE(Source.find("factor = "), std::string::npos) << Source;
  EXPECT_FALSE(std::regex_search(
      Source, std::regex("\\b0x" + llvm::utohexstr(GOT->Addr) + "\\b")))
      << Source;
}

// `clang_call_handler` calls through its argument after its call/pop, with no
// stack argument.  The return address the call/pop pushed lies below the
// stack pointer of the call: it was popped, and is no argument.  The call had
// passed it, as libc++'s `std::__terminate` passed its PC to the terminate
// handler: `func(0x1CA9B0)`.
TEST(X86_32PICLibrary, APoppedGetPcPushIsNoCallArgument) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *Pop = Img->findSymbol("clang_call_handler_pop");
  ASSERT_NE(Pop, nullptr);
  const std::string Source = decompile(*Img, "clang_call_handler");
  EXPECT_FALSE(std::regex_search(
      Source, std::regex("\\b0x" + llvm::utohexstr(Pop->Addr) + "\\b")))
      << Source;
  EXPECT_TRUE(std::regex_search(Source, std::regex("\\)\\(\\);"))) << Source;
  EXPECT_NE(Source.find("factor = 1"), std::string::npos) << Source;
}

// `clang_spill_call`, built at -O0, spills both its arguments and the GOT
// address to its frame (`mov [esp+0x14], edx` ...) and calls through
// `clang_hook` with no stack argument.  The stack pointer the call is made
// with places each spill above the outgoing area: none is a push.  The call
// had passed them, `clang_hook(arg0, arg1, 0x3788)` in reverse store order,
// as android_x86_server's `get_idc_filename` passed its spills to
// `get_lex_vars`.
TEST(X86_32PICLibrary, ASpillAboveTheOutgoingAreaIsNoPush) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *GOT = Img->findSymbol("_GLOBAL_OFFSET_TABLE_");
  ASSERT_NE(GOT, nullptr);
  const std::string Source = decompile(*Img, "clang_spill_call");
  EXPECT_FALSE(std::regex_search(
      Source, std::regex("\\b0x" + llvm::utohexstr(GOT->Addr) + "\\b")))
      << Source;
  EXPECT_TRUE(std::regex_search(Source, std::regex("\\)\\(\\);"))) << Source;
}

// `clang_pass_result` stores the result of its first call at 4(%esp), one
// slot past a first slot it never writes, and calls through its table again.
// That store is no spill: the second call keeps the first one's result.
TEST(X86_32PICLibrary, AResultPastAnEmptyFirstSlotStaysAnArgument) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source = decompile(*Img, "clang_pass_result");
  // The first call's result is named and passed to the second call.
  std::smatch Result;
  ASSERT_TRUE(std::regex_search(
      Source, Result, std::regex(R"((\w+) = \(\(uint32_t \(\*\)\(\)\))")))
      << Source;
  EXPECT_TRUE(
      std::regex_search(Source, std::regex("\\(" + Result[1].str() + "\\);")))
      << Source;
}

// `clang_call_keep_got`, built at -O0, spills the GOT address to 4(%esp),
// one slot past the empty first one, calls through its argument with no
// argument and reloads the address to read `factor`.  The spill is the GOT
// address only its reload reads, no argument: the call had passed it.
TEST(X86_32PICLibrary, AGOTSpillIsNoCallArgument) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *GOT = Img->findSymbol("_GLOBAL_OFFSET_TABLE_");
  ASSERT_NE(GOT, nullptr);
  const std::string Source = decompile(*Img, "clang_call_keep_got");
  EXPECT_FALSE(std::regex_search(
      Source, std::regex("\\b0x" + llvm::utohexstr(GOT->Addr) + "\\b")))
      << Source;
  EXPECT_TRUE(std::regex_search(Source, std::regex("\\)\\(\\);"))) << Source;
  EXPECT_NE(Source.find("factor"), std::string::npos) << Source;
}

// `clang_set_factor_o0`, built at -O0, spills the GOT address and returns
// with its reload in %eax.  The reload is the GOT address by the spill proof,
// no value the function means: the C returns nothing.
TEST(X86_32PICLibrary, AReloadedGOTAddressLeftInTheReturnRegisterIsNoValue) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *GOT = Img->findSymbol("_GLOBAL_OFFSET_TABLE_");
  ASSERT_NE(GOT, nullptr);
  const std::string Source = decompile(*Img, "clang_set_factor_o0");
  EXPECT_NE(Source.find("void clang_set_factor_o0("), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("factor = "), std::string::npos) << Source;
  EXPECT_FALSE(std::regex_search(
      Source, std::regex("\\b0x" + llvm::utohexstr(GOT->Addr) + "\\b")))
      << Source;
}

// `clang_ui_value` reads its result four bytes from the GOT's spill, after a
// call that pops its hidden pointer.  The stack model does not know the
// callee pops, so the read looks like the GOT's reload.  `clang_ui_caller`
// stores what a call to it through its PLT entry returns, so the function
// keeps a value and the caller stores it.
TEST(X86_32PICLibrary, AReloadedGOTAddressACallerReadsStaysAValue) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source =
      decompile(*Img, "clang_ui_value", {"clang_ui_caller"});
  EXPECT_NE(Source.find("int32_t clang_ui_value("), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("void clang_ui_value("), std::string::npos) << Source;
  EXPECT_TRUE(std::regex_search(Source, std::regex("= clang_ui_value\\(\\);")))
      << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

// `clang_forward_factor` returns with what its call of `clang_set_factor`
// through the PLT left in %eax.  The C calls `clang_set_factor`, which returns
// nothing, so the forwarder returns nothing either.
TEST(X86_32PICLibrary, AVoidRoutineCalledThroughItsPLTEntryHandsNothingBack) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source =
      decompile(*Img, "clang_forward_factor", {"clang_set_factor"});
  EXPECT_NE(Source.find("void clang_forward_factor("), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("clang_set_factor(arg0);"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

// `clang_maybe_scale` returns nothing, and %eax merges what its paths left
// there: a call's result and the GOT address.  The C prints a bare return,
// so it assigns neither to a local: the call runs as a statement.
TEST(X86_32PICLibrary, WhatAVoidFunctionsReturnMergesIsNoAssignment) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const Symbol *GOT = Img->findSymbol("_GLOBAL_OFFSET_TABLE_");
  ASSERT_NE(GOT, nullptr);
  const std::string Source = decompile(*Img, "clang_maybe_scale");
  const size_t Begin = Source.find("void clang_maybe_scale(");
  ASSERT_NE(Begin, std::string::npos) << Source;
  const std::string Body =
      Source.substr(Begin, Source.find("\n}\n", Begin) - Begin);
  EXPECT_TRUE(
      std::regex_search(Body, std::regex("\\n\\s*clang_scale_three\\(\\);")))
      << Source;
  EXPECT_FALSE(std::regex_search(Body, std::regex("\\bv[0-9_]+ ="))) << Source;
  EXPECT_FALSE(std::regex_search(
      Body, std::regex("\\b0x" + llvm::utohexstr(GOT->Addr) + "\\b")))
      << Source;
}

// `clang_nothrow_call`'s landing pad stores the exception object at (%esp) and
// calls __clang_call_terminate.  The unwinder enters the pad with the stack
// pointer of the call it protects, so the store is the call's argument, and
// the pad is the catch-all that terminates a noexcept function.  The call
// had no argument, and the C passed an unknown value.
TEST(X86_32PICLibrary, ALandingPadPassesTheExceptionObjectOnTheStack) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source = decompile(*Img, "clang_nothrow_call");
  EXPECT_TRUE(std::regex_search(
      Source, std::regex("catch \\(\\.\\.\\.\\) \\{\\s*std_terminate\\(\\);")))
      << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

// `clang_catch_what`'s landing pad passes the exception object to
// __cxa_begin_catch and _Unwind_Resume on the stack, and calls the caught
// object's `what` through its vtable with that object alone.  The pad is
// entered with the stack pointer of the call it protects: before, both
// runtime calls passed an unknown value, and the virtual call took the
// caught object's local copy for a second argument.
TEST(X86_32PICLibrary, ACatchHandlerPassesWhatItsPadStores) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  const std::string Source = decompile(*Img, "clang_catch_what");
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  std::smatch Call;
  ASSERT_TRUE(std::regex_search(Source, Call,
                                std::regex("\\+ 8\\)\\)\\)\\(([^()]*)\\);")))
      << Source;
  EXPECT_EQ(Call[1].str().find(','), std::string::npos) << Source;
}

// A linked library reads a `.rodata` table at its GOTOFF displacement from the
// GOT plus an index.  The LLVM module took the GOT for the table's base: GCC's
// read went into the writable run that holds the GOT, at a negative offset,
// and clang's read dereferenced the table's original address.  Both read the
// table's own read-only global now.
TEST(X86_32PICLibrary, GOTRelativeTableReadsItsGlobal) {
  const std::optional<BinaryImage> Img = loadLibrary();
  if (!Img)
    GTEST_SKIP() << "the fixture links with ld.lld";
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  const PipelineResult Result = Pipeline().run(*Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  for (const char *Name : {"gcc_lookup", "clang_lookup"}) {
    SCOPED_TRACE(Name);
    const llvm::Function *Lifted = Result.LlvmModule->getFunction(Name);
    ASSERT_NE(Lifted, nullptr);
    unsigned TableLoads = 0;
    for (const llvm::BasicBlock &Block : *Lifted)
      for (const llvm::Instruction &Inst : Block) {
        EXPECT_FALSE(llvm::isa<llvm::IntToPtrInst>(Inst));
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Inst);
        if (!Load)
          continue;
        const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(
            llvm::getUnderlyingObject(Load->getPointerOperand()));
        ASSERT_NE(Global, nullptr);
        EXPECT_TRUE(Global->isConstant());
        ++TableLoads;
      }
    EXPECT_EQ(TableLoads, 1u);
  }
}

// The object those functions are linked from gives the pop's sum the GOT
// base, not the PC: `clang_scale`'s pop is read only by that sum, and its
// push stayed a local (`var_m24 = 0x7E;`).
TEST(X86_32PICLibrary, ObjectGetPcCallPushIsNoLocal) {
  const std::optional<BinaryImage> Img = loadFixture("test_pic_library_i386.o");
  if (!Img)
    GTEST_SKIP() << "the fixture assembles with clang";
  expectNoGetPcPush(*Img);
}

} // namespace

//===- Win64CallContractTests.cpp - Microsoft x64 import arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// MSVC calls a C runtime import through its import address slot
// (`call [__imp_fputs]`); MinGW calls a thunk that jumps there
// (`jmp [__imp_fputs]`).  A wrapper that passes its own RCX and RDX on leaves
// them unwritten.  The import's prototype says it reads them, so they are the
// wrapper's parameters and the call's arguments.
//
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "WindowsQualifiedFixture.h"
#include "WindowsVariadicFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x140001000;
constexpr va_t Iat = 0x140003000;

void put(std::vector<uint8_t> &Code, va_t At, std::vector<uint8_t> Bytes) {
  std::copy(Bytes.begin(), Bytes.end(), Code.begin() + (At - Text));
}

std::vector<uint8_t> rel32(va_t Next, va_t To) {
  const uint32_t Delta = static_cast<uint32_t>(To - Next);
  return {static_cast<uint8_t>(Delta), static_cast<uint8_t>(Delta >> 8),
          static_cast<uint8_t>(Delta >> 16), static_cast<uint8_t>(Delta >> 24)};
}

/// The slot of import \p Index.
constexpr va_t slot(size_t Index) { return Iat + 8 * Index; }

/// `call [rip + Slot]`, or `jmp` when \p Jump, at \p At.
std::vector<uint8_t> throughSlot(va_t At, va_t Slot, bool Jump = false) {
  std::vector<uint8_t> Bytes = {0xFF, static_cast<uint8_t>(Jump ? 0x25 : 0x15)};
  for (uint8_t B : rel32(At + 6, Slot))
    Bytes.push_back(B);
  return Bytes;
}

/// `sub rsp, 28h`, the calls \p Calls, `add rsp, 28h` and `ret`: a function
/// that passes its own argument registers on to each callee.  The first call
/// starts 4 bytes in.
std::vector<uint8_t> framed(const std::vector<std::vector<uint8_t>> &Calls) {
  std::vector<uint8_t> Bytes = {0x48, 0x83, 0xEC, 0x28};
  for (const auto &Call : Calls)
    Bytes.insert(Bytes.end(), Call.begin(), Call.end());
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x28, 0xC3})
    Bytes.push_back(B);
  return Bytes;
}

/// A PE32+ image with \p Code at Text, the given function symbols, and the
/// msvcrt.dll imports \p Imports in consecutive slots at Iat.
BinaryImage
makeImage(std::vector<uint8_t> Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<const char *> &Imports) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Iat;
  Idata.Size = Idata.FileSz = 8 * Imports.size();
  Idata.Data.resize(Idata.Size);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Imports[I];
    Imp.IATAddr = slot(I);
    Img.Imports.push_back(std::move(Imp));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}

PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                   std::set<va_t> Entries) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  return Result;
}

/// HighC for the functions at \p Entries of \p Img.
std::string liftEntries(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, std::move(Entries));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The definition of function \p Name in \p Source: the line naming it that
/// opens a body, not a declaration before it.
std::string body(const std::string &Source, const std::string &Name) {
  const std::string Head = " " + Name + "(";
  for (size_t At = Source.find(Head); At != std::string::npos;
       At = Source.find(Head, At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || Source[LineEnd - 1] != '{')
      continue;
    const size_t Close = Source.find("\n}", LineEnd);
    if (Close == std::string::npos)
      return {};
    const size_t Start = Source.rfind('\n', At) + 1;
    return Source.substr(Start, Close + 2 - Start);
  }
  return {};
}

unsigned family(uint64_t RegOff) { return static_cast<unsigned>(RegOff / 8); }

TEST(Win64CallContract, CpuidScratchExchangeAddsNoPhantomParameters) {
  constexpr va_t Query = Text + 0x40;
  std::vector<uint8_t> Code(0x80, 0xCC);
  std::vector<uint8_t> Call = {0xE8};
  for (uint8_t B : rel32(Text + 9, Query))
    Call.push_back(B);
  put(Code, Text, framed({Call}));
  put(Code, Query, {0x31, 0xC0,             // xor eax, eax
                    0x31, 0xC9,             // xor ecx, ecx
                    0x4C, 0x87, 0xC3,       // xchg rbx, r8
                    0x0F, 0xA2,             // cpuid
                    0x4C, 0x87, 0xC3,       // xchg rbx, r8
                    0x44, 0x31, 0xC0,       // xor eax, r8d
                    0x31, 0xC8, 0x31, 0xD0, // xor eax, ecx; xor eax, edx
                    0xC3});                 // ret
  const BinaryImage Img =
      makeImage(Code, {{Text, "query_wrapper"}, {Query, "cpu_query"}}, {});
  for (bool IncludeCallee : {false, true}) {
    SCOPED_TRACE(IncludeCallee);
    const std::set<va_t> Entries =
        IncludeCallee ? std::set<va_t>{Text, Query} : std::set<va_t>{Text};
    const std::string Source = liftEntries(Img, Entries);
    EXPECT_NE(Source.find("query_wrapper(void)"), std::string::npos) << Source;
    EXPECT_NE(Source.find("cpu_query()"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
    if (IncludeCallee)
      EXPECT_NE(Source.find("cpu_query(void)"), std::string::npos) << Source;

    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.LiftMode = true;
    Opts.SourceProjection = true;
    Opts.NoOpt = true;
    Opts.OnlyFunctionEntries = Entries;
    auto Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    if (IncludeCallee) {
      auto *Callee = Result.LlvmModule->getFunction("cpu_query");
      ASSERT_NE(Callee, nullptr);
      EXPECT_EQ(Callee->arg_size(), 0u);
    }
    auto *Caller = Result.LlvmModule->getFunction("query_wrapper");
    ASSERT_NE(Caller, nullptr);
    EXPECT_EQ(Caller->arg_size(), 0u);
    unsigned Calls = 0;
    for (auto &Block : *Caller)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
            Call && (!Call->getCalledFunction() ||
                     !Call->getCalledFunction()->isIntrinsic())) {
          ++Calls;
          EXPECT_EQ(Call->arg_size(), 0u);
        }
    EXPECT_EQ(Calls, 1u);
  }
}

TEST(Win64CallContract, ACallThroughTheImportSlotPassesTheCallersArguments) {
  // wrap(s, f) calls fputs through its slot without writing RCX or RDX.
  constexpr va_t Wrap = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Wrap, framed({throughSlot(Wrap + 4, slot(0))}));
  const BinaryImage Img = makeImage(Code, {{Wrap, "wrap"}}, {"fputs"});
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(
      Body.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, LLVMImportCallKeepsUnwrittenLeadingArgument) {
  // run_pipe(command): RCX is forwarded, only the second argument is set.
  constexpr va_t Wrap = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Wrap,
      framed({{0xBA, 0x45, 0x23, 0x01, 0x00}, throughSlot(Wrap + 9, slot(0))}));
  BinaryImage Img = makeImage(Code, {{Wrap, "wrap"}}, {"_popen"});
  ASSERT_TRUE(Img.recordImportStorageSlot(slot(0), "_popen", 0,
                                          ImportStorageEvidence::PointerTable));
  for (bool NoOpt : {true, false}) {
    SCOPED_TRACE(NoOpt);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.LiftMode = true;
    Opts.SourceProjection = true;
    Opts.NoOpt = NoOpt;
    Opts.OnlyFunctionEntries = {Wrap};
    auto Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    auto *Caller = Result.LlvmModule->getFunction("wrap");
    auto *Callee = Result.LlvmModule->getFunction("_popen");
    ASSERT_NE(Caller, nullptr);
    ASSERT_NE(Callee, nullptr);
    ASSERT_EQ(Caller->arg_size(), 1u);
    ASSERT_EQ(Callee->arg_size(), 2u);
    unsigned Calls = 0;
    for (auto &Block : *Caller)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
            Call && Call->getCalledFunction() == Callee) {
          ++Calls;
          EXPECT_EQ(Call->getFunctionType(), Callee->getFunctionType());
          ASSERT_EQ(Call->arg_size(), 2u);
          EXPECT_EQ(Call->getArgOperand(0), Caller->getArg(0));
          auto *Second =
              llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(1));
          ASSERT_NE(Second, nullptr);
          EXPECT_EQ(Second->getZExtValue(), 0x12345u);
        }
    EXPECT_EQ(Calls, 1u);
  }
}

TEST(Win64CallContract, SavedXmmRegistersDoNotShiftTheFifthParameter) {
  // Sum five integer arguments while saving/restoring nonvolatile XMM6/7.
  // The save slots are frame state, never extra parameters between R9 and
  // the incoming stack argument.
  const std::vector<uint8_t> Code{
      0x48, 0x83, 0xEC, 0x48,       // sub rsp,72
      0x0F, 0x29, 0x74, 0x24, 0x20, // movaps [rsp+32],xmm6
      0x0F, 0x29, 0x7C, 0x24, 0x30, // movaps [rsp+48],xmm7
      0x66, 0x0F, 0xEF, 0xF6,       // pxor xmm6,xmm6
      0x66, 0x0F, 0xEF, 0xFF,       // pxor xmm7,xmm7
      0x48, 0x8B, 0x44, 0x24, 0x70, // mov rax,[rsp+112]
      0x48, 0x01, 0xC8,             // add rax,rcx
      0x48, 0x01, 0xD0,             // add rax,rdx
      0x4C, 0x01, 0xC0,             // add rax,r8
      0x4C, 0x01, 0xC8,             // add rax,r9
      0x0F, 0x28, 0x74, 0x24, 0x20, // movaps xmm6,[rsp+32]
      0x0F, 0x28, 0x7C, 0x24, 0x30, // movaps xmm7,[rsp+48]
      0x48, 0x83, 0xC4, 0x48, 0xC3};
  const auto Image = makeImage(Code, {{Text, "sum_five"}}, {});
  for (bool NoOpt : {true, false}) {
    SCOPED_TRACE(NoOpt);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = true;
    Options.SourceProjection = true;
    Options.NoOpt = NoOpt;
    Options.OnlyFunctionEntries = {Text};
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    auto *Function = Result.LlvmModule->getFunction("sum_five");
    ASSERT_NE(Function, nullptr);
    ASSERT_EQ(Function->arg_size(), 5u);
    for (const auto &Argument : Function->args())
      EXPECT_TRUE(Argument.getType()->isIntegerTy(64));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, {}));
    const std::string Driver = R"(
int main(void) {
  for (uint64_t i = 0; i != 1024; ++i) {
    uint64_t a = i * UINT64_C(0x9e3779b97f4a7c15);
    uint64_t b = ~a, c = i << 32, d = i * i, e = (i + 1) << 40;
    if (sum_five(a, b, c, d, e) != a + b + c + d + e) return 1;
  }
  return 0;
}
)";
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      source_call_execution_test::compileAndRun(
          Source + Driver,
          {Optimization, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(Win64CallContract, AThunkJumpingThroughTheSlotForwardsTheArguments) {
  // A MinGW thunk `jmp [__imp_fputs]`, called by wrap(s, f) without writing
  // RCX or RDX: both read the parameters fputs reads.
  constexpr va_t Wrap = Text, Thunk = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> Call = {0xE8};
  for (uint8_t B : rel32(Wrap + 4 + 5, Thunk))
    Call.push_back(B);
  put(Code, Wrap, framed({Call}));
  put(Code, Thunk, throughSlot(Thunk, slot(0), /*Jump=*/true));
  const BinaryImage Img =
      makeImage(Code, {{Wrap, "wrap"}, {Thunk, "fputs_thunk"}}, {"fputs"});
  const std::string Source = liftEntries(Img, {Wrap, Thunk});
  const std::string ThunkBody = body(Source, "fputs_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(
      ThunkBody.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << ThunkBody;
  const std::string WrapBody = body(Source, "wrap");
  ASSERT_FALSE(WrapBody.empty()) << Source;
  EXPECT_NE(WrapBody.find("fputs_thunk(arg0, arg1)"), std::string::npos)
      << WrapBody;
  for (const std::string &Body : {ThunkBody, WrapBody})
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, AThunkForwardsTheStackArgumentsItsImportReads) {
  // A MinGW thunk `jmp [__imp___getmainargs]`, called by wrap(a, b, c, d, e)
  // with its own arguments: __getmainargs reads a fifth on the stack, which
  // wrap stores at [rsp+20h] and the thunk leaves where its caller put it.
  constexpr va_t Wrap = Text, Thunk = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> Body = {
      0x48, 0x83, 0xEC, 0x38,       // sub rsp, 38h
      0x48, 0x8B, 0x44, 0x24, 0x60, // mov rax, [rsp+60h]
      0x48, 0x89, 0x44, 0x24, 0x20, // mov [rsp+20h], rax
      0xE8};                        // call Thunk
  for (uint8_t B : rel32(Wrap + Body.size() + 4, Thunk))
    Body.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x38, 0xC3}) // add rsp, 38h; ret
    Body.push_back(B);
  put(Code, Wrap, Body);
  put(Code, Thunk, throughSlot(Thunk, slot(0), /*Jump=*/true));
  BinaryImage Img = makeImage(
      Code, {{Wrap, "wrap"}, {Thunk, "getmainargs_thunk"}}, {"__getmainargs"});
  // The PE loader records the thunk as the import's stub.
  ASSERT_TRUE(Img.recordImportStub(Thunk, 0));
  const std::string Source = liftEntries(Img, {Wrap, Thunk});
  const std::string ThunkBody = body(Source, "getmainargs_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(ThunkBody.find("arg4)"), std::string::npos) << ThunkBody;
  const std::string WrapBody = body(Source, "wrap");
  ASSERT_FALSE(WrapBody.empty()) << Source;
  // A call to the stub names the import.
  EXPECT_NE(WrapBody.find("__getmainargs("), std::string::npos) << WrapBody;
  EXPECT_NE(WrapBody.find("arg4)"), std::string::npos) << WrapBody;
  for (const std::string &Body : {ThunkBody, WrapBody})
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, OwnedX87ContextKeepsCallSetupAndTailArguments) {
  // Win64 owns the x87 stack as one context and publishes it around every
  // call. That transport neither ends the stores that set a call's stack
  // arguments nor separates a tail jump from its return.
  constexpr va_t Caller = Text, Thunk = Text + 0x60;
  std::vector<uint8_t> Code(0x80, 0xCC);
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x48,                         // sub rsp, 48h
      0xD9, 0xE8, 0xDD, 0xD8,                         // fld1; fstp st(0)
      0x48, 0xC7, 0x44, 0x24, 0x20, 0xF5, 0x01, 0, 0, // mov [rsp+20h], 501
      0x48, 0xC7, 0x44, 0x24, 0x28, 0x5A, 0x02, 0, 0, // mov [rsp+28h], 602
      0x48, 0xC7, 0x44, 0x24, 0x30, 0xBF, 0x02, 0, 0, // mov [rsp+30h], 703
      0x48, 0xC7, 0x44, 0x24, 0x38, 0x24, 0x03, 0, 0, // mov [rsp+38h], 804
      0x41, 0xB9, 4,    0,    0,    0,                // mov r9d, 4
      0x41, 0xB8, 3,    0,    0,    0,                // mov r8d, 3
      0xBA, 2,    0,    0,    0,                      // mov edx, 2
      0xB9, 1,    0,    0,    0};                     // mov ecx, 1
  const auto Call = throughSlot(Caller + CallerCode.size(), slot(0));
  CallerCode.insert(CallerCode.end(), Call.begin(), Call.end());
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x48, 0xC3}) // add rsp, 48h; ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  // The thunk changes x87 state, then jumps to the import with its own
  // caller's arguments, four of them in that caller's outgoing stack area.
  std::vector<uint8_t> ThunkCode = {0xD9, 0xE8, 0xDD, 0xD8}; // fld1; fstp st(0)
  const auto Jump =
      throughSlot(Thunk + ThunkCode.size(), slot(0), /*Jump=*/true);
  ThunkCode.insert(ThunkCode.end(), Jump.begin(), Jump.end());
  put(Code, Thunk, ThunkCode);
  const BinaryImage Img = makeImage(
      Code, {{Caller, "caller"}, {Thunk, "x87_thunk"}}, {"RtlVirtualUnwind"});
  const std::string Source = liftEntries(Img, {Caller, Thunk});
  EXPECT_NE(Source.find("neverd_x87_"), std::string::npos) << Source;
  const std::string CallerBody = body(Source, "caller");
  ASSERT_FALSE(CallerBody.empty()) << Source;
  EXPECT_NE(CallerBody.find("RtlVirtualUnwind("), std::string::npos)
      << CallerBody;
  // 501, 602, 703 and 804, as C writes numbers by default.
  for (const char *Value : {"0x1F5", "0x25A", "0x2BF", "0x324"})
    EXPECT_NE(CallerBody.find(Value), std::string::npos) << Value << '\n'
                                                         << CallerBody;
  const std::string ThunkBody = body(Source, "x87_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(ThunkBody.find("arg7)"), std::string::npos) << ThunkBody;
  for (const std::string &Text : {CallerBody, ThunkBody})
    EXPECT_EQ(Text.find("unknown value"), std::string::npos) << Text;
}

TEST(Win64CallContract, TheRuntimesOwnPrototypeFixesWhatItsSlotReads) {
  // One function calls each import through its slot.  calloc reads RCX and
  // RDX by the C library's arity, _initterm by the Windows C runtime's own
  // prototype, and _lock reads RCX alone as that runtime declares it.
  // __getmainargs reads all four registers and a fifth argument on the
  // stack.  _pipe(fds, size, mode) has no fixed reads: POSIX pipe(fds) is
  // another function.
  const std::vector<const char *> Imports = {"calloc", "_initterm", "_lock",
                                             "__getmainargs", "_pipe"};
  constexpr va_t Caller = Text;
  std::vector<uint8_t> Code(0x48, 0xCC);
  std::vector<std::vector<uint8_t>> Calls;
  for (size_t I = 0; I < Imports.size(); ++I)
    Calls.push_back(throughSlot(Caller + 4 + 6 * I, slot(I)));
  put(Code, Caller, framed(Calls));
  const BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, Imports);
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Caller});
  const auto &Reads = Result.CallEntryReadGPRs;
  for (size_t I = 0; I < 2; ++I) {
    SCOPED_TRACE(Imports[I]);
    ASSERT_TRUE(Reads.count(slot(I)));
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::RCX)], 8u);
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::RDX)], 8u);
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::R8)], 0u);
  }
  ASSERT_TRUE(Reads.count(slot(2)));
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::RDX)], 0u);
  ASSERT_TRUE(Reads.count(slot(3)));
  EXPECT_EQ(Reads.at(slot(3))[family(x86reg::R9)], 8u);
  // Stack arguments count positions, the registers' included.
  ASSERT_TRUE(Result.CallEntryStackArgs.count(slot(3)));
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(3)), 5);
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(0)), 0);
  EXPECT_FALSE(Reads.count(slot(4)));
}

TEST(Win64CallContract, CallersPassWhatAnExportedRoutinesPrototypeDeclares) {
  // The image exports a system call stub that reads only RCX itself: the
  // kernel's dispatcher reads the rest of the five parameters its prototype
  // declares, the fifth from the stack.  It also exports a routine whose
  // body reads R9 past the three parameters its prototype declares, which
  // its summary keeps.
  constexpr va_t Caller = Text, Stub = Text + 0x30, Extra = Text + 0x40;
  std::vector<uint8_t> Code(0x48, 0xCC);
  // sub rsp, 38h; mov qword [rsp+20h], 0; mov r9d, 4; lea r8, [rsp+30h];
  // mov edx, 12; mov rcx, -2; call Stub; add rsp, 38h; ret
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x38, 0x48, 0xC7, 0x44, 0x24, 0x20, 0x00,
      0x00, 0x00, 0x00, 0x41, 0xB9, 0x04, 0x00, 0x00, 0x00, 0x4C,
      0x8D, 0x44, 0x24, 0x30, 0xBA, 0x0C, 0x00, 0x00, 0x00, 0x48,
      0xC7, 0xC1, 0xFE, 0xFF, 0xFF, 0xFF, 0xE8};
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, Stub))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x38, 0xC3})
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  // mov r10, rcx; mov eax, 5; ret
  put(Code, Stub, {0x4C, 0x8B, 0xD1, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3});
  // mov rax, r9; ret
  put(Code, Extra, {0x4C, 0x89, 0xC8, 0xC3});
  BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, {});
  Img.Exports.push_back({"QueryThing", 1, Stub});
  Img.Exports.push_back({"AllocateThing", 2, Extra});
  class ExportedPrototypes : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
      if (Addr != Stub && Addr != Extra)
        return std::nullopt;
      FunctionSym Function;
      Function.Addr = Addr;
      Function.PrototypeLibrary = "windows-user-x64";
      const TypeRef Pointer = NdType::makePtr(NdType::makeVoid());
      if (Addr == Stub) {
        Function.Name = "QueryThing";
        Function.ReturnType = NdType::makeInt(4, true);
        Function.Params = {{"Handle", Pointer},
                           {"Class", NdType::makeInt(4, true)},
                           {"Buffer", Pointer},
                           {"Length", NdType::makeInt(4)},
                           {"Returned", Pointer}};
      } else {
        Function.Name = "AllocateThing";
        Function.ReturnType = Pointer;
        Function.Params = {{"Heap", Pointer},
                           {"Flags", NdType::makeInt(4)},
                           {"Size", NdType::makeInt(8)}};
      }
      return Function;
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Caller, Extra};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts, &Dbg);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.CallEntryStackArgs.count(Stub));
  EXPECT_EQ(Result.CallEntryStackArgs.at(Stub), 5);
  // Each parameter is read as wide as the prototype declares it, which a
  // caller setting only the low bytes of an argument register fills.
  const auto &Reads = Result.CallEntryReadGPRs;
  ASSERT_TRUE(Reads.count(Stub));
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::RDX)], 4u);
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::R8)], 8u);
  EXPECT_EQ(Reads.at(Stub)[family(x86reg::R9)], 4u);
  ASSERT_TRUE(Reads.count(Extra));
  EXPECT_EQ(Reads.at(Extra)[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(Extra)[family(x86reg::RDX)], 4u);
  EXPECT_EQ(Reads.at(Extra)[family(x86reg::R9)], 8u);
}

TEST(Win64CallContract, TheStartupsKernel32CallsReadWhatTheyDeclare) {
  // MSVC's startup code calls these through their slots.  IsDebuggerPresent
  // takes no argument, GetModuleHandleW one, and RtlVirtualUnwind four in
  // registers and four on the stack.
  const std::vector<const char *> Imports = {
      "IsDebuggerPresent", "GetModuleHandleW", "RtlVirtualUnwind"};
  constexpr va_t Caller = Text;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<std::vector<uint8_t>> Calls;
  for (size_t I = 0; I < Imports.size(); ++I)
    Calls.push_back(throughSlot(Caller + 4 + 6 * I, slot(I)));
  put(Code, Caller, framed(Calls));
  const BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, Imports);
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Caller});
  const auto &Reads = Result.CallEntryReadGPRs;
  if (Reads.count(slot(0)))
    for (unsigned Bytes : Reads.at(slot(0)))
      EXPECT_EQ(Bytes, 0u);
  ASSERT_TRUE(Reads.count(slot(1)));
  EXPECT_EQ(Reads.at(slot(1))[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(slot(1))[family(x86reg::RDX)], 0u);
  ASSERT_TRUE(Reads.count(slot(2)));
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::R9)], 8u);
  ASSERT_TRUE(Result.CallEntryStackArgs.count(slot(2)));
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(2)), 8);
  const std::string Source = body(liftEntries(Img, {Caller}), "caller");
  EXPECT_NE(Source.find("IsDebuggerPresent();"), std::string::npos) << Source;
}

TEST(Win64CallContract, ACallNamesTheFunctionNotItsObjectsSection) {
  // MinGW links each object's code with the section's own symbol where the
  // object's first function starts: `.text` comes first in the symbol table,
  // then _setargv.  A call from a function decompiled alone names _setargv,
  // as the function decompiled alone is named.
  constexpr va_t Caller = Text;
  constexpr va_t Callee = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> Call = {0xE8}; // call rel32
  for (uint8_t B : rel32(Caller + 9, Callee))
    Call.push_back(B);
  put(Code, Caller, framed({Call}));
  put(Code, Callee, {0x31, 0xC0, 0xC3}); // xor eax, eax; ret
  BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, {"calloc"});
  Symbol Section;
  Section.Name = ".text";
  Section.Addr = Callee;
  Img.Symbols.push_back(std::move(Section));
  Symbol Function;
  Function.Name = "_setargv";
  Function.Addr = Callee;
  Function.IsFunc = true;
  Img.Symbols.push_back(std::move(Function));
  ASSERT_EQ(Img.getFunctionNameAt(Callee), "_setargv");
  const std::string Source = body(liftEntries(Img, {Caller}), "caller");
  EXPECT_NE(Source.find("_setargv("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("text("), std::string::npos) << Source;
}

TEST(Win64CallContract, ADataImportReadsAsItsSlot) {
  // MinGW reads msvcrt's _commode through `.refptr.__imp__commode`, a
  // pointer to the import's slot in read-only data; MSVC reads the slot
  // itself.  Both read the address the loader binds to the slot.
  constexpr va_t Refptr = 0x140004000;
  constexpr va_t ViaRefptr = Text;
  constexpr va_t Direct = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> Indirect = {0x48, 0x8B, 0x05}; // mov rax, [rip + Refptr]
  for (uint8_t B : rel32(ViaRefptr + 7, Refptr))
    Indirect.push_back(B);
  for (uint8_t B : {0x48, 0x8B, 0x00, 0xC3}) // mov rax, [rax]; ret
    Indirect.push_back(B);
  put(Code, ViaRefptr, Indirect);
  std::vector<uint8_t> Slot = {0x48, 0x8B, 0x05}; // mov rax, [rip + slot]
  for (uint8_t B : rel32(Direct + 7, slot(0)))
    Slot.push_back(B);
  Slot.push_back(0xC3); // ret
  put(Code, Direct, Slot);
  BinaryImage Img = makeImage(
      Code, {{ViaRefptr, "p_commode"}, {Direct, "read_commode"}}, {"_commode"});
  Segment Rdata;
  Rdata.Name = ".rdata";
  Rdata.VA = Refptr;
  Rdata.Size = Rdata.FileSz = 8;
  Rdata.Flags = SegmentFlags::Readable;
  const va_t SlotVA = slot(0);
  Rdata.Data.resize(8);
  std::memcpy(Rdata.Data.data(), &SlotVA, sizeof(SlotVA));
  Img.Segments.push_back(std::move(Rdata));
  const std::string Source = liftEntries(Img, {ViaRefptr, Direct});
  EXPECT_NE(Source.find("extern void *__imp__commode;"), std::string::npos)
      << Source;
  for (const char *Name : {"p_commode", "read_commode"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    EXPECT_NE(Body.find("__imp__commode;"), std::string::npos) << Source;
    EXPECT_EQ(Body.find("0x140003000"), std::string::npos) << Source;
  }
  EXPECT_EQ(Source.find("refptr"), std::string::npos) << Source;
}

TEST(Win64CallContract, AFloatArgumentTakesItsSlotsVectorRegister) {
  // scale(x, n) = x * n reads x in xmm0, the first slot's vector register,
  // and n in edx, the second slot's integer one. twice(x) = x + x;
  // quadruple(x) doubles twice(x), passing x on in xmm0.
  constexpr va_t Scale = Text, Twice = Text + 0x10, Quadruple = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  put(Code, Scale,
      {0xF3, 0x0F, 0x2A, 0xCA, // cvtsi2ss xmm1, edx
       0xF3, 0x0F, 0x59, 0xC1, // mulss xmm0, xmm1
       0xC3});                 // ret
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
       0xC3});                 // ret
  std::vector<uint8_t> QuadrupleCode = {0x48, 0x83, 0xEC, 0x28, // sub rsp, 28h
                                        0xE8};                  // call twice
  for (uint8_t B : rel32(Quadruple + 9, Twice))
    QuadrupleCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                    0xC3})                  // ret
    QuadrupleCode.push_back(B);
  put(Code, Quadruple, QuadrupleCode);
  const BinaryImage Img = makeImage(
      Code, {{Scale, "scale"}, {Twice, "twice"}, {Quadruple, "quadruple"}}, {});
  const std::string Source = liftEntries(Img, {Scale, Twice, Quadruple});
  EXPECT_NE(Source.find("float scale(float arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  const std::string Body = body(Source, "quadruple");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double quadruple(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}

TEST(Win64CallContract, AFloatArgumentReachesItsImportsSlot) {
  // sin_twice(x) = sin(x + x) through the import's slot: sin's double
  // parameter takes the first slot's vector register.
  constexpr va_t SinTwice = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> SinTwiceCode = {0x48, 0x83, 0xEC, 0x28,  // sub rsp, 28h
                                       0xF2, 0x0F, 0x58, 0xC0}; // addsd
  for (uint8_t B : throughSlot(SinTwice + 8, slot(0)))
    SinTwiceCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                    0xC3})                  // ret
    SinTwiceCode.push_back(B);
  put(Code, SinTwice, SinTwiceCode);
  const BinaryImage Img = makeImage(Code, {{SinTwice, "sin_twice"}}, {"sin"});
  const std::string Body = body(liftEntries(Img, {SinTwice}), "sin_twice");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double sin_twice(double arg0)"), std::string::npos)
      << Body;
  // ADD's numerical bits and MXCSR complete together before the import.
  EXPECT_NE(Body.find("neverd_x86_fp_add_state_f64("), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return sin("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}

TEST(Win64CallContract,
     HeaderPointerQualifiersAgreeWithIndependentDefinitions) {
  auto Image = makeImage(throughSlot(Text, Iat, true),
                         {{Text, "forward_qualified"}}, {"fixture_qualified"});
  ASSERT_TRUE(Image.recordImportStorageSlot(Iat, "fixture_qualified", 0,
                                            ImportStorageEvidence::LoaderBind));
  for (bool Nested : {false, true})
    for (bool Record : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Nested);
        SCOPED_TRACE(Record);
        SCOPED_TRACE(LLVM);
        windows_qualified_test::Header Debug;
        Debug.Slot = Iat;
        Debug.Nested = Nested;
        Debug.Record = Record;
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        Options.OnlyFunctionEntries = {Text};
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                          &Debug, &Image));
        } else {
          ASSERT_TRUE(
              HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
        }
        // Independent C definitions require the same qualifiers, including
        // those behind a pointer and on an opaque record's returned pointer.
        const std::string Element =
            Record ? "struct fixture_record" : "uint16_t";
        const std::string Definition =
            "\nstruct fixture_record { uint16_t value; };\nconst " + Element +
            " *fixture_qualified(const " + Element +
            (Nested ? " *const *input" : " *input") +
            ", volatile uint32_t *output) {\n"
            "  if (*output != 41) __builtin_trap();\n"
            "  *output = 42; return " +
            (Nested ? "*input" : "input") + ";\n}\nint main(void) {\n  " +
            Element + " value = {0x1234};\n  const " + Element +
            " *input = &value;\n  uint32_t output = 41;\n"
            "  uintptr_t result = forward_qualified((uintptr_t)" +
            (Nested ? "&input" : "input") +
            ", (uintptr_t)&output);\n"
            "  return result != (uintptr_t)&value || output != 42;\n}\n";
        source_call_execution_test::compileAndRun(Source + Definition);
      }
}

TEST(Win64CallContract, HeaderVariadicPrefixKeepsEveryProvenTailArgument) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Iat;
  for (unsigned Tail : {0u, 1u, 3u}) {
    // RCX passes through untouched; only the count and variadic values are set.
    std::vector<uint8_t> Setup = {0xBA, static_cast<uint8_t>(Tail), 0, 0, 0};
    if (Tail >= 1)
      Setup.insert(Setup.end(), {0x41, 0xB8, 11, 0, 0, 0});
    if (Tail >= 3) {
      Setup.insert(Setup.end(), {0x41, 0xB9, 22, 0, 0, 0});
      Setup.insert(Setup.end(), {0xC7, 0x44, 0x24, 0x20, 33, 0, 0, 0});
    }
    for (bool Writable : {false, true}) {
      for (bool RegisterTarget : {false, true}) {
        const va_t At = Text + 4 + Setup.size();
        auto Call = throughSlot(At, Iat);
        if (RegisterTarget) {
          Call = {0x48, 0x8B, 0x05};
          const auto Displacement = rel32(At + 7, Iat);
          Call.insert(Call.end(), Displacement.begin(), Displacement.end());
          Call.insert(Call.end(), {0xFF, 0xD0});
        }
        BinaryImage Image =
            makeImage(framed({Setup, Call}), {{Text, "forward_variadic"}},
                      {"fixture_variadic"});
        ASSERT_TRUE(Image.recordImportStorageSlot(
            Iat, "fixture_variadic", 0, ImportStorageEvidence::LoaderBind));
        if (Writable)
          Image.Segments.back().Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(Tail);
          SCOPED_TRACE(RegisterTarget);
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(Writable);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.OnlyFunctionEntries.insert(Text);
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            const auto *Callee =
                Result.LlvmModule->getFunction("fixture_variadic");
            ASSERT_NE(Callee, nullptr);
            EXPECT_TRUE(Callee->isVarArg());
            ASSERT_EQ(Callee->arg_size(), 2u);
            EXPECT_TRUE(Callee->getReturnType()->isIntegerTy(32));
            EXPECT_TRUE(
                Callee->getFunctionType()->getParamType(0)->isIntegerTy(32));
            EXPECT_TRUE(
                Callee->getFunctionType()->getParamType(1)->isIntegerTy(32));
            if (Writable && RegisterTarget) {
              const auto *Forward =
                  Result.LlvmModule->getFunction("forward_variadic");
              ASSERT_NE(Forward, nullptr);
              bool Indirect = false;
              for (const auto &Block : *Forward)
                for (const auto &Instruction : Block)
                  if (const auto *Call =
                          llvm::dyn_cast<llvm::CallBase>(&Instruction);
                      Call && Call->isIndirectCall()) {
                    Indirect = true;
                    EXPECT_EQ(Call->getFunctionType(),
                              Callee->getFunctionType());
                    EXPECT_EQ(Call->arg_size(), 2u + Tail);
                  }
              EXPECT_TRUE(Indirect);
            }
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Debug, &Image));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
          }
          source_call_execution_test::compileAndRun(
              Source + windows_variadic_test::oracle(Tail, "forward_variadic"));
        }
      }
    }
  }
}

TEST(Win64CallContract, HeaderVariadicFixedPrefixCanOverflowToTheStack) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Iat;
  Debug.Fixed = 5;
  auto Image = makeImage(throughSlot(Text, Iat, true), {{Text, "forward_many"}},
                         {"fixture_variadic"});
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    Options.OnlyFunctionEntries = {Text};
    auto Result = Pipeline().run(Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs.front().Params.size(), 5u);
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions, &Debug,
                                      &Image));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
    }
    source_call_execution_test::compileAndRun(Source + R"(
int32_t fixture_variadic(int32_t first, int32_t count, int32_t fixed2,
                        int32_t fixed3, int32_t fixed4, ...) {
  if (first != -7 || count != 8 || fixed2 != 9 || fixed3 != 10 || fixed4 != 11)
    __builtin_trap();
  return -19;
}
int main(void) { return (int32_t)forward_many(-7, 8, 9, 10, 11) != -19; }
)");
  }
}

TEST(Win64CallContract, HeaderOnlyFloatingImportRetainsItsTypedABI) {
  class Header : public NullDebugContext {
  public:
    unsigned Bytes = 8;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_fp")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeFloat(Bytes);
      Function.Params = {{"x", NdType::makeFloat(Bytes)},
                         {"y", NdType::makeFloat(Bytes)}};
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-windows-header";
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Result =
          Address == Iat ? resolvePrototype("fixture_fp") : std::nullopt;
      if (Result)
        Result->Addr = Address;
      return Result;
    }
  } Debug;
  for (bool RegisterTarget : {false, true}) {
    for (bool Writable : {false, true}) {
      std::vector<uint8_t> Call = throughSlot(Text + 4, Iat);
      if (RegisterTarget) {
        Call = {0x48, 0x8B, 0x05}; // mov rax, [rip + Iat]; call rax
        const auto Displacement = rel32(Text + 4 + 7, Iat);
        Call.insert(Call.end(), Displacement.begin(), Displacement.end());
        Call.insert(Call.end(), {0xFF, 0xD0});
      }
      BinaryImage Image =
          makeImage(framed({Call}), {{Text, "forward_fp"}}, {"fixture_fp"});
      ASSERT_TRUE(Image.recordImportStorageSlot(
          Iat, "fixture_fp", 0, ImportStorageEvidence::LoaderBind));
      if (Writable)
        Image.Segments.back().Flags =
            SegmentFlags::Readable | SegmentFlags::Writable;
      for (unsigned Bytes : {4u, 8u}) {
        Debug.Bytes = Bytes;
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(RegisterTarget);
          SCOPED_TRACE(Writable);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.LiftMode = LLVM;
          Options.SourceProjection = LLVM;
          Options.OnlyFunctionEntries.insert(Text);
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            const auto *Callee = Result.LlvmModule->getFunction("fixture_fp");
            std::string IR;
            llvm::raw_string_ostream IROut(IR);
            Result.LlvmModule->print(IROut, nullptr);
            ASSERT_NE(Callee, nullptr) << IR;
            ASSERT_EQ(Callee->arg_size(), 2u);
            const auto *Type = Bytes == 4 ? llvm::Type::getFloatTy(Context)
                                          : llvm::Type::getDoubleTy(Context);
            EXPECT_EQ(Callee->getReturnType(), Type);
            EXPECT_EQ(Callee->getFunctionType()->getParamType(0), Type);
            EXPECT_EQ(Callee->getFunctionType()->getParamType(1), Type);
            EXPECT_EQ(Callee->getCallingConv(), llvm::CallingConv::Win64);
            if (Writable && RegisterTarget) {
              const auto *Forward =
                  Result.LlvmModule->getFunction("forward_fp");
              ASSERT_NE(Forward, nullptr);
              bool Indirect = false;
              for (const auto &Block : *Forward)
                for (const auto &Instruction : Block)
                  if (const auto *Call =
                          llvm::dyn_cast<llvm::CallBase>(&Instruction);
                      Call && Call->isIndirectCall()) {
                    Indirect = true;
                    EXPECT_EQ(Call->getFunctionType(),
                              Callee->getFunctionType());
                  }
              EXPECT_TRUE(Indirect) << IR;
            }
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                            &Debug, &Image));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
          }
          const std::string Type = Bytes == 4 ? "float" : "double";
          std::string Oracle =
              "\n" + Type + " fixture_fp(" + Type + " x, " + Type +
              " y) {\n"
              "  if (x != 1.25 || y != -2.5) __builtin_trap();\n"
              "  return 7.75;\n}\n";
          if (LLVM) {
            // The machine LLVM entry retains complete vector register carriers.
            // Its external import must still consume/produce scalar FP values.
            Oracle +=
                "typedef uint64_t carrier __attribute__((vector_size(16)));\n"
                "int main(void) {\n"
                "  carrier x = {0, 0}, y = {0, 0};\n  " +
                Type +
                " a = 1.25, b = -2.5, result;\n"
                "  __builtin_memcpy(&x, &a, sizeof(a));\n"
                "  __builtin_memcpy(&y, &b, sizeof(b));\n"
                "  carrier out = forward_fp(x, y);\n"
                "  __builtin_memcpy(&result, &out, sizeof(result));\n"
                "  return result != 7.75;\n}\n";
          } else {
            Oracle +=
                "int main(void) { return forward_fp(1.25, -2.5) != 7.75; }\n";
          }
          source_call_execution_test::compileAndRun(Source + Oracle);
        }
      }
    }
  }
}
} // namespace

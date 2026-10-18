//===- AAPCS64CallContractTests.cpp - AArch64 call arguments -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 code reaches another module's routine through the pointer it loads
// from the slot the loader binds to that routine: a PLT entry's `adrp x16;
// ldr x17, [x16, #slot]; add x16, x16, #slot; br x17`, or a call built with
// -fno-plt.  The routine's prototype gives the arguments it takes, which a
// function passing its own on takes as its parameters.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x10000;
constexpr va_t Plt = 0x10100;
constexpr va_t Got = 0x20000;

uint32_t adrp(unsigned Rd, va_t At, va_t Target) {
  const uint32_t Pages =
      static_cast<uint32_t>((Target >> 12) - (At >> 12)) & 0x1FFFFF;
  return 0x90000000u | (Pages & 3) << 29 | (Pages >> 2) << 5 | Rd;
}
uint32_t ldrX(unsigned Rt, unsigned Rn, va_t Slot) {
  return 0xF9400000u | static_cast<uint32_t>(Slot & 0xFFF) / 8 << 10 | Rn << 5 |
         Rt;
}
uint32_t addX(unsigned Rd, unsigned Rn, va_t Slot) {
  return 0x91000000u | static_cast<uint32_t>(Slot & 0xFFF) << 10 | Rn << 5 | Rd;
}
uint32_t br(unsigned Rn) { return 0xD61F0000u | Rn << 5; }
uint32_t blr(unsigned Rn) { return 0xD63F0000u | Rn << 5; }
constexpr uint32_t PushFrame = 0xA9BF7BFDu; // stp x29, x30, [sp, #-16]!
constexpr uint32_t SetFrame = 0x910003FDu;  // mov x29, sp
constexpr uint32_t AddW0One = 0x11000400u;  // add w0, w0, #1
constexpr uint32_t PopFrame = 0xA8C17BFDu;  // ldp x29, x30, [sp], #16
constexpr uint32_t Ret = 0xD65F03C0u;

/// The canonical PLT entry at \p At for the import in \p Slot.
std::vector<uint32_t> pltEntry(va_t At, va_t Slot) {
  return {adrp(16, At, Slot), ldrX(17, 16, Slot), addX(16, 16, Slot), br(17)};
}

void place(Segment &Seg, va_t At, const std::vector<uint32_t> &Words) {
  for (size_t I = 0; I < Words.size(); ++I)
    for (unsigned B = 0; B < 4; ++B)
      Seg.Data[At - Seg.VA + I * 4 + B] =
          static_cast<uint8_t>(Words[I] >> (8 * B));
}

Segment codeSegment(const char *Name, va_t VA, size_t Size) {
  Segment Seg;
  Seg.Name = Name;
  Seg.VA = VA;
  Seg.Size = Seg.FileSz = Size;
  Seg.Data.assign(Size, 0);
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  return Seg;
}

/// An AArch64 ELF image with \p Code placed at its addresses, the function
/// symbols \p Functions, and \p Imports bound to consecutive GOT slots from
/// Got.  The loader binds the slot of a symbol no module of the image
/// defines; one the image defines (\p Defined) it relocates to the image's
/// own routine.  The PLT entries pair with their imports as the loader pairs
/// them.
BinaryImage
makeImage(const std::vector<std::pair<va_t, std::vector<uint32_t>>> &Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<const char *> &Imports, bool Defined = false) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment TextSeg = codeSegment(".text", Text, Plt - Text);
  Segment PltSeg = codeSegment(".plt", Plt, 0x100);
  for (const auto &[At, Words] : Code)
    place(At < Plt ? TextSeg : PltSeg, At, Words);
  Img.Segments.push_back(std::move(TextSeg));
  Img.Segments.push_back(std::move(PltSeg));
  Segment GotSeg;
  GotSeg.Name = ".got";
  GotSeg.VA = Got;
  GotSeg.Size = GotSeg.FileSz = 8 * Imports.size();
  GotSeg.Data.assign(GotSeg.Size, 0);
  GotSeg.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(GotSeg));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Img.Imports.push_back({"libc.so", Imports[I], 0, Got + 8 * I});
    if (!Defined)
      EXPECT_TRUE(Img.recordImportStorageSlot(
          Got + 8 * I, Imports[I], 0, ImportStorageEvidence::LoaderBind));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  scanImportThunks(Img);
  return Img;
}

PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                   std::set<va_t> Entries, bool LLVM = false) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  Opts.LiftMode = LLVM;
  return Pipeline().run(Img, Ctx, Opts);
}

/// HighC for the functions at \p Entries of \p Img.
std::string highC(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, std::move(Entries));
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The text of function \p Name's definition, its signature included.
std::string body(const std::string &Source, const std::string &Name) {
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos)
    return {};
  const size_t Open = Source.find('{', At);
  const size_t Close = Source.find("\n}", Open);
  if (Open == std::string::npos || Close == std::string::npos)
    return {};
  const size_t Begin = Source.rfind('\n', At) + 1;
  return Source.substr(Begin, Close + 2 - Begin);
}

TEST(AAPCS64CallContract, APLTEntryPassesTheArgumentsOfItsImport) {
  // islower takes one argument, which its PLT entry passes on in x0 without
  // writing it; getuid takes none.
  constexpr va_t Lower = Plt, Uid = Plt + 0x10;
  const BinaryImage Img =
      makeImage({{Lower, pltEntry(Lower, Got)}, {Uid, pltEntry(Uid, Got + 8)}},
                {}, {"islower", "getuid"});
  ASSERT_NE(Img.findImportAt(Lower), nullptr);
  const std::string Source = highC(Img, {Lower, Uid});
  const std::string LowerBody = body(Source, "sub_10100");
  ASSERT_FALSE(LowerBody.empty()) << Source;
  EXPECT_NE(LowerBody.find("sub_10100(int64_t arg0)"), std::string::npos)
      << LowerBody;
  EXPECT_NE(LowerBody.find("islower(arg0)"), std::string::npos) << LowerBody;
  EXPECT_EQ(LowerBody.find("unknown value"), std::string::npos) << LowerBody;
  const std::string UidBody = body(Source, "sub_10110");
  ASSERT_FALSE(UidBody.empty()) << Source;
  EXPECT_NE(UidBody.find("sub_10110(void)"), std::string::npos) << UidBody;
  EXPECT_NE(UidBody.find("getuid()"), std::string::npos) << UidBody;
}

TEST(AAPCS64CallContract, PLTEntriesOfVariadicSystemCallsPassTheirArgsOn) {
  // syscall reads at most six arguments after the number, ptrace three after
  // the request and prctl four after the option.  Each entry passes every
  // argument on, which C does one by one.
  constexpr va_t Sys = Plt, Trace = Plt + 0x10, Control = Plt + 0x20;
  const BinaryImage Img = makeImage({{Sys, pltEntry(Sys, Got)},
                                     {Trace, pltEntry(Trace, Got + 8)},
                                     {Control, pltEntry(Control, Got + 16)}},
                                    {}, {"syscall", "ptrace", "prctl"});
  const std::string Source = highC(Img, {Sys, Trace, Control});
  EXPECT_NE(Source.find("long sub_10100(long arg0, ...) {"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return syscall(arg0, argument1, argument2, "
                        "argument3, argument4, argument5, argument6);"),
            std::string::npos)
      << Source;
  // glibc's request is an enumeration, which <sys/ptrace.h> declares.
  EXPECT_NE(Source.find("#include <sys/ptrace.h>"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("long sub_10110(enum __ptrace_request arg0, ...) {"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return ptrace(arg0, argument1, argument2, "
                        "argument3);"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("int sub_10120(int arg0, ...) {"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return prctl(arg0, argument1, argument2, argument3, "
                        "argument4);"),
            std::string::npos)
      << Source;
}

TEST(AAPCS64CallContract, PLTEntriesPassArgumentsOnAsTheirCLibraryDeclares) {
  // Bionic's ptrace and ioctl take their requests as ints, which glibc's
  // headers declare an enumeration and an unsigned long: the entries declare
  // them as <sys/ptrace.h> and <sys/ioctl.h> do.
  constexpr va_t Trace = Plt, Control = Plt + 0x10;
  BinaryImage Img = makeImage(
      {{Trace, pltEntry(Trace, Got)}, {Control, pltEntry(Control, Got + 8)}},
      {}, {"ptrace", "ioctl"});
  Img.ELFMetadata.emplace();
  Img.ELFMetadata->Interpreter = "/system/bin/linker64";
  const std::string Source = highC(Img, {Trace, Control});
  EXPECT_NE(Source.find("extern long ptrace(int, ...);"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("long sub_10100(int arg0, ...) {"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("__ptrace_request"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern int ioctl(int, int, ...);"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("int sub_10110(int arg0, int arg1, ...) {"),
            std::string::npos)
      << Source;
}

TEST(AAPCS64CallContract, APLTEntryInLLVMCCallsItsImportByName) {
  // The entry calls the word it loads from islower's slot.  Its function is
  // not islower, which it calls by that name as <ctype.h> declares it; the
  // word is no value of the C.
  const BinaryImage Img =
      makeImage({{Plt, pltEntry(Plt, Got)}}, {}, {"islower"});
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Error;
  const auto Module = llvm::parseAssemblyString(R"(
target triple = "aarch64-unknown-linux-gnu"
@__nd_codeptr_20000 = external global i64
define i64 @sub_10100(i64 %arg0) {
  %ld = load volatile i64, ptr @__nd_codeptr_20000, align 8
  %fn = inttoptr i64 %ld to ptr
  %call = tail call i64 %fn(i64 %arg0)
  ret i64 %call
}
)",
                                                Error, Ctx);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options, nullptr, &Img,
                                  Module->getFunction("sub_10100")));
  EXPECT_NE(Source.find("#include <ctype.h>"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint64_t sub_10100(uint64_t arg0) {"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return islower(arg0);"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("volatile"), std::string::npos) << Source;
}

TEST(AAPCS64CallContract, OnlyReachingEntryRegistersBecomeForwardedParameters) {
  for (unsigned Mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(Mode);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::ELF;
    MedFunc F;
    F.Entry = Text;
    F.CC = CallingConv::ARM_AAPCS;
    F.Blocks.emplace_back();
    F.Blocks.front().Id = 0;
    F.Blocks.front().StartAddr = Text;
    F.Blocks.front().EndAddr = Text + 16;
    auto Call = [](va_t Target) {
      MedOp Op;
      Op.Opcode = NdOp::CALL;
      Op.addInput(MedVar::makeConst(Target, 8));
      return Op;
    };
    if (Mode == 1)
      F.Blocks.front().Ops.push_back(Call(Text + 0x80));
    if (Mode >= 2) {
      F.Blocks.emplace_back();
      F.Blocks.back().Id = 1;
      F.Blocks.back().StartAddr = Text + 32;
      F.Blocks.back().EndAddr = Text + 48;
      if (Mode == 2)
        F.ModuleAnalysisRoots.insert(Text + 32);
      else
        F.Blocks.back().ExceptionalPreds.emplace_back();
    }
    F.Blocks.back().Ops.push_back(Call(Text + 0x40));
    std::map<va_t, int> Regs{{Text + 0x40, 3}}, Total = Regs;
    recoverCallAbi(F, Image.Arch, {}, &Image, &Regs, &Total);
    ASSERT_FALSE(F.CallInfos.empty());
    const auto &C = F.CallInfos.back();
    ASSERT_EQ(C.Args.size(), 3u);
    if (Mode == 0) {
      EXPECT_EQ(F.Params.size(), 3u);
      for (const auto &V : C.Args)
        EXPECT_EQ(V.Kind, MedVar::Param);
    } else {
      EXPECT_TRUE(F.Params.empty());
      for (const auto &V : C.Args)
        EXPECT_EQ(V.Kind, MedVar::Unspecified);
    }
  }
}

TEST(AAPCS64CallContract,
     DirectCalleesPrecedeDeepLowerAddressChainsWithinBudget) {
  constexpr va_t First = 0x10000, Zero = 0x20000, One = 0x20040,
                 Caller = 0x30000;
  auto BL = [](va_t At, va_t Target) {
    return 0x94000000u | (uint32_t((Target - At) / 4) & 0x03ffffffu);
  };
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  auto Text = codeSegment(".text", First, Caller - First + 0x100);
  auto Add = [&](va_t At, const char *Name, std::vector<uint32_t> Words) {
    place(Text, At, Words);
    auto S = Symbol::makeFunc(At);
    S.Name = Name;
    S.Size = Words.size() * 4;
    Image.Symbols.push_back(std::move(S));
  };
  for (size_t I = 0; I <= limits::kMaxCallEffectExtraLifts; ++I) {
    const auto At = First + I * 32;
    if (I != limits::kMaxCallEffectExtraLifts)
      Add(At, "", {PushFrame, SetFrame, BL(At + 8, At + 32), PopFrame, Ret});
    else
      Add(At, "", {0xD2800120u, Ret}); // mov x0, 9
  }
  Add(Zero, "takes_none", {0xD28000e0u, Ret}); // mov x0, 7
  Add(One, "takes_one", {0xF900001fu, Ret});   // str xzr, [x0]
  Add(Caller, "selected_caller",
      {PushFrame, SetFrame, BL(Caller + 8, First), BL(Caller + 12, Zero),
       BL(Caller + 16, One), PopFrame, Ret});
  Image.Segments.push_back(std::move(Text));
  llvm::LLVMContext Context;
  const auto Result = run(Image, Context, {Caller});
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.OmittedCallees);
  std::set<va_t> Summarized;
  for (const auto &F : Result.OmittedCallees->Bodies)
    Summarized.insert(F.Entry);
  EXPECT_TRUE(Summarized.contains(Zero));
  EXPECT_TRUE(Summarized.contains(One));
  EXPECT_LE(Summarized.size(), limits::kMaxCallEffectExtraLifts);
  const auto &Function = Result.MedFuncs.front();
  unsigned Checked = 0;
  for (const auto &Call : Function.CallInfos)
    if (Call.TargetAddr == Zero || Call.TargetAddr == One) {
      EXPECT_EQ(Call.Args.size(), Call.TargetAddr == Zero ? 0u : 1u);
      ++Checked;
    }
  EXPECT_EQ(Checked, 2u);
}

/// lower(c) tail-calls islower and bump(c) returns toupper(c) + 1, each
/// through the GOT slot the call loads, as -fno-plt builds them.
BinaryImage makeGOTCalls() {
  constexpr va_t Lower = Text, Bump = Text + 0x10;
  constexpr va_t LowerSlot = Got, BumpSlot = Got + 8;
  return makeImage(
      {{Lower, {adrp(1, Lower, LowerSlot), ldrX(1, 1, LowerSlot), br(1)}},
       {Bump,
        {PushFrame, SetFrame, adrp(8, Bump + 8, BumpSlot), ldrX(8, 8, BumpSlot),
         blr(8), AddW0One, PopFrame, Ret}}},
      {{Lower, "lower"}, {Bump, "bump"}}, {"islower", "toupper"});
}

TEST(AAPCS64CallContract, ACallThroughAGOTSlotPassesTheIncomingArgument) {
  const BinaryImage Img = makeGOTCalls();
  const std::string Source = highC(Img, {Text, Text + 0x10});
  const std::string Lower = body(Source, "lower");
  ASSERT_FALSE(Lower.empty()) << Source;
  EXPECT_NE(Lower.find("lower(int64_t arg0)"), std::string::npos) << Lower;
  EXPECT_NE(Lower.find("islower(arg0)"), std::string::npos) << Lower;
  const std::string Bump = body(Source, "bump");
  ASSERT_FALSE(Bump.empty()) << Source;
  EXPECT_NE(Bump.find("toupper(arg0)"), std::string::npos) << Bump;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

TEST(AAPCS64CallContract, TheLLVMRoutePassesTheArgumentThroughAGOTSlot) {
  // The call through the slot passes the function's own argument, whether
  // the emitter calls the import or the word it loads.
  const BinaryImage Img = makeGOTCalls();
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Text, Text + 0x10}, true);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  for (const char *Name : {"lower", "bump"}) {
    SCOPED_TRACE(Name);
    const llvm::Function *Fn = Result.LlvmModule->getFunction(Name);
    ASSERT_NE(Fn, nullptr);
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    Fn->print(OS);
    ASSERT_EQ(Fn->arg_size(), 1u) << IR;
    const llvm::CallInst *Call = nullptr;
    for (const llvm::BasicBlock &Block : *Fn)
      for (const llvm::Instruction &I : Block)
        if (const auto *C = llvm::dyn_cast<llvm::CallInst>(&I);
            C && !llvm::isa<llvm::IntrinsicInst>(C))
          Call = C;
    ASSERT_NE(Call, nullptr) << IR;
    ASSERT_EQ(Call->arg_size(), 1u) << IR;
    EXPECT_EQ(Call->getArgOperand(0)->stripPointerCasts(), Fn->getArg(0)) << IR;
  }
}

/// A shared object that defines g(x) { return x + x; } and calls it through
/// its PLT entry, as `f: b g@plt` does for a preemptible g.  The loader
/// relocates g's slot to g itself.
constexpr va_t OwnG = Text, OwnF = Text + 0x10, OwnStub = Plt;
BinaryImage makeOwnRoutinePLT(const char *Import = "g",
                              const char *Alias = nullptr) {
  constexpr uint32_t AddW0W0 = 0x0B000000u; // add w0, w0, w0
  const uint32_t BranchToStub =
      0x14000000u | (static_cast<uint32_t>(OwnStub - OwnF) >> 2 & 0x3FFFFFF);
  std::vector<std::pair<va_t, const char *>> Functions = {{OwnG, "g"},
                                                          {OwnF, "f"}};
  if (Alias)
    Functions.push_back({OwnG, Alias});
  BinaryImage Img = makeImage({{OwnG, {AddW0W0, Ret}},
                               {OwnF, {BranchToStub}},
                               {OwnStub, pltEntry(OwnStub, Got)}},
                              Functions, {Import},
                              /*Defined=*/true);
  for (unsigned B = 0; B < 8; ++B)
    Img.Segments.back().Data[B] = static_cast<uint8_t>(OwnG >> (8 * B));
  Img.CodePtrRelocSlots.insert(Got);
  return Img;
}

TEST(AAPCS64CallContract,
     ASlotTheLoaderStartsAtThisImagesRoutinePassesItsArgs) {
  // The PLT entry passes g's one argument on, to g by its name.
  const BinaryImage Img = makeOwnRoutinePLT();
  const std::string Source = highC(Img, {OwnG, OwnStub});
  const std::string Entry = body(Source, "sub_10100");
  ASSERT_FALSE(Entry.empty()) << Source;
  EXPECT_NE(Entry.find("sub_10100(int64_t arg0)"), std::string::npos) << Entry;
  EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]g\\(arg0\\)").match(Entry)) << Entry;
  EXPECT_EQ(Entry.find("unknown value"), std::string::npos) << Entry;
}

TEST(AAPCS64CallContract, ACallThroughAnAliasesSlotKeepsTheAliasesName) {
  // libgcc's __lttf2 and __letf2 name one routine: a call through the slot
  // the loader binds to one of them names the routine by that name.
  const BinaryImage Img = makeOwnRoutinePLT("galias", "galias");
  const std::string Source = highC(Img, {OwnG, OwnF, OwnStub});
  for (const char *Name : {"f", "sub_10100"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]galias\\(arg0\\)").match(Body))
        << Body;
  }
}

TEST(AAPCS64CallContract, ACallToThePLTEntryOfItsOwnRoutinePassesItsArgs) {
  // f reaches g through g's PLT entry and passes its argument on, whether
  // or not the entry and g are decompiled with it.
  const BinaryImage Img = makeOwnRoutinePLT();
  for (const std::set<va_t> &Entries :
       {std::set<va_t>{OwnG, OwnF, OwnStub}, std::set<va_t>{OwnF}}) {
    SCOPED_TRACE(Entries.size());
    const std::string Source = highC(Img, Entries);
    const std::string F = body(Source, "f");
    ASSERT_FALSE(F.empty()) << Source;
    EXPECT_NE(F.find("f(int64_t arg0)"), std::string::npos) << F;
    EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]g\\(arg0\\)").match(F)) << F;
    EXPECT_FALSE(llvm::Regex("[^[:alnum:]_]g_[[:xdigit:]]+\\(").match(Source))
        << Source;
  }
  // The LLVM route passes it too, rather than a value of its own: inlined
  // or not, what f returns depends on its argument.
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {OwnG, OwnF}, true);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  const llvm::Function *Fn = Result.LlvmModule->getFunction("f");
  ASSERT_NE(Fn, nullptr);
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Fn->print(OS);
  ASSERT_EQ(Fn->arg_size(), 1u) << IR;
  EXPECT_FALSE(Fn->getArg(0)->use_empty()) << IR;
  for (const llvm::BasicBlock &Block : *Fn)
    for (const llvm::Instruction &I : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call))
        for (const llvm::Value *Argument : Call->args())
          EXPECT_FALSE(llvm::isa<llvm::Constant>(Argument)) << IR;
}

} // namespace

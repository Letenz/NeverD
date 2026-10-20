//===- ResumableTrapBoundaryTests.cpp - Debugger trap boundaries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check function ownership at resumable traps without suppressing real
/// debugger continuations or ordinary transfers into another function.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t Entry = 0x1000;
constexpr va_t Next = Entry + 6;

BinaryImage image(Arch Architecture = Arch::X64,
                  BinaryFormat Format = BinaryFormat::COFF) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = Format;
  Image.Base = Entry;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0xb8, 7,  0, 0, 0, // mov eax, 7
               0xcc,              // int3
               0xb8, 42, 0, 0, 0, // mov eax, 42
               0xc3};             // ret
  Text.Size = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  return Image;
}

void addSymbol(BinaryImage &Image, va_t Address, uint64_t Size,
               const char *Name) {
  auto Sym = Symbol::makeFunc(Address, Size);
  Sym.Name = Name;
  Image.Symbols.push_back(std::move(Sym));
}

LowFunc build(const BinaryImage &Image, const std::set<va_t> &Entries = {}) {
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  return Builder.build(Image, Dec, Entry, "trap_owner");
}

void expectStoppedAtTrap(const LowFunc &Function,
                         Arch Architecture = Arch::X64) {
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_FALSE(lowFunctionNeverReturns(Function, Architecture));
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(Function.Blocks.size(), 1u);
  const auto &Block = Function.Blocks.front();
  EXPECT_EQ(Block.EndAddr, Next);
  EXPECT_TRUE(Block.Succs.empty());
  ASSERT_EQ(Block.InstructionBoundaries.size(), 2u);
  const auto &Trap = Block.InstructionBoundaries.back();
  EXPECT_EQ(Trap.Address, Next - 1);
  EXPECT_EQ(Trap.Control, LowInstructionControl::Terminator);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Trap.ControlFlags, LowInstructionControlFlag::Resumable));
  for (const auto &Op : Block.Ops) {
    EXPECT_NE(Op.Opcode, NdOp::CALL);
    EXPECT_NE(Op.Opcode, NdOp::RETURN);
  }
}

TEST(ResumableTrapBoundary, SizedMetadataStopsBeforeDecodableUnownedBytes) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool SizedSymbol : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Architecture) << "/"
                     << static_cast<int>(Format) << "/" << SizedSymbol);
        auto Image = image(Architecture, Format);
        if (SizedSymbol)
          addSymbol(Image, Entry, Next - Entry, "trap_owner");
        else
          Image.KnownCodeRanges.emplace_back(Entry, Next);
        expectStoppedAtTrap(build(Image), Architecture);
      }
}

TEST(ResumableTrapBoundary, AdjacentFunctionEntryDoesNotBecomeATailCall) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (unsigned Evidence = 0; Evidence != 4; ++Evidence) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Architecture) << "/" << Evidence);
      auto Image = image(Architecture);
      std::set<va_t> Entries;
      switch (Evidence) {
      case 0:
        Entries.insert(Next);
        break;
      case 1:
        Image.KnownCodeRanges.emplace_back(Next, Next + 6);
        break;
      case 2:
        addSymbol(Image, Next, 0, "neighbor");
        break;
      case 3:
        Image.RuntimeFunctionAddrs.insert(Next);
        break;
      }
      expectStoppedAtTrap(build(Image, Entries), Architecture);
    }
}

TEST(ResumableTrapBoundary, UnwindEndStopsWithoutAnAdjacentEntry) {
  auto Image = image();
  ExceptionFunction Owner;
  Owner.CodeRange = {Entry, Next};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  Image.ExceptionMetadata.Functions.push_back(std::move(Owner));
  Image.ExceptionMetadata.rebuildIndex();
  expectStoppedAtTrap(build(Image));
}

TEST(ResumableTrapBoundary, InteriorDebugBreakRetainsItsContinuation) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (bool Bounded : {false, true}) {
      auto Image = image(Architecture);
      if (Bounded)
        Image.KnownCodeRanges.emplace_back(Entry, Next + 6);
      const auto Function = build(Image);
      EXPECT_TRUE(Function.hasCompleteLiftCoverage());
      EXPECT_EQ(Function.DecodedInstructionCount, 4u);
      ASSERT_EQ(Function.Blocks.size(), 1u);
      EXPECT_EQ(Function.Blocks.front().EndAddr, Next + 6);
      EXPECT_EQ(Function.Blocks.front().InstructionBoundaries.back().Control,
                LowInstructionControl::Return);
    }
}

TEST(ResumableTrapBoundary, AdjacentOwnedFragmentRetainsItsContinuation) {
  auto Image = image();
  Image.KnownCodeRanges = {{Entry, Next}, {Next, Next + 6}};
  ExceptionFunction Owner;
  Owner.CodeRange = {Entry, Next};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  Owner.FragmentRanges.push_back({Next, Next + 6});
  Image.ExceptionMetadata.Functions.push_back(std::move(Owner));
  Image.ExceptionMetadata.rebuildIndex();
  const auto Function = build(Image, {Entry, Next});
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 4u);
  ASSERT_EQ(Function.Blocks.size(), 1u);
  EXPECT_EQ(Function.Blocks.front().EndAddr, Next + 6);
}

TEST(ResumableTrapBoundary, OrdinaryFallthroughStillCallsAdjacentFunction) {
  auto Image = image();
  Image.Segments.front().Data[5] = 0x90; // nop instead of int3
  Image.KnownCodeRanges = {{Entry, Next}, {Next, Next + 6}};
  const auto Function = build(Image);
  unsigned Calls = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::TailCall) {
        EXPECT_EQ(Boundary.Immediate, Next);
        ++Calls;
      }
  EXPECT_EQ(Calls, 1u);
}

TEST(ResumableTrapBoundary, ExplicitTailJumpAfterDebugBreakIsPreserved) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    auto Image = image(Architecture);
    auto &Text = Image.Segments.front();
    Text.Data.resize(13, 0xcc);
    Text.Data[6] = 0xeb; // jmp Entry + 12
    Text.Data[7] = 4;
    Text.Data[12] = 0xc3;
    Text.Size = Text.Data.size();
    Image.KnownCodeRanges = {{Entry, Entry + 8}, {Entry + 12, Entry + 13}};
    const auto Function = build(Image);
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, 3u);
    ASSERT_EQ(Function.Blocks.size(), 1u);
    const auto &Boundaries = Function.Blocks.front().InstructionBoundaries;
    ASSERT_EQ(Boundaries.size(), 3u);
    EXPECT_EQ(Boundaries[1].Control, LowInstructionControl::Terminator);
    EXPECT_EQ(Boundaries[2].Control, LowInstructionControl::TailCall);
    EXPECT_EQ(Boundaries[2].Immediate, Entry + 12);
  }
}

TEST(ResumableTrapBoundary, BothSourceRoutesKeepTrapWithoutCallingNeighbor) {
  for (bool LLVMRoute : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(::testing::Message() << LLVMRoute << "/" << NoOpt);
      auto Image = image();
      Image.KnownCodeRanges = {{Entry, Next}, {Next, Next + 6}};
      addSymbol(Image, Entry, Next - Entry, "trap_owner");
      addSymbol(Image, Next, 6, "neighbor");
      // A returning call followed by int3 must keep both operations, without
      // adding a call to the independent function immediately after the trap.
      auto &Text = Image.Segments.front();
      Text.Data[0] = 0xe8; // call Entry + 0x10
      Text.Data[1] = 0x0b;
      Text.Data.resize(0x10, 0xcc);
      // mov dword ptr [rcx], 7; ret -- the call has an observable effect even
      // when the enclosing function does not consume its return register.
      Text.Data.insert(Text.Data.end(), {0xc7, 0x01, 7, 0, 0, 0, 0xc3});
      Text.Size = Text.Data.size();
      addSymbol(Image, Entry + 0x10, 7, "returning_helper");
      Image.KnownCodeRanges.emplace_back(Entry + 0x10, Entry + 0x17);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries.insert(Entry);
      Options.LiftMode = LLVMRoute;
      Options.SourceProjection = LLVMRoute;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      if (LLVMRoute) {
        ASSERT_TRUE(Result.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions,
                                        nullptr, &Image));
      } else {
        ASSERT_FALSE(Result.HighFuncs.empty());
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
      }
      EXPECT_NE(Source.find("__debugbreak()"), std::string::npos) << Source;
      EXPECT_NE(Source.find("returning_helper("), std::string::npos) << Source;
      EXPECT_EQ(Source.find("neighbor("), std::string::npos) << Source;
    }
}

struct TrapCase {
  const char *Name;
  Arch Architecture;
  InstructionMode Mode;
  std::vector<uint8_t> Trap;
  std::vector<uint8_t> Continuation;
  bool Resumable;
};

std::vector<TrapCase> siblingTraps() {
  std::vector<TrapCase> Cases;
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    for (const auto &[Name, Bytes] :
         std::vector<std::pair<const char *, std::vector<uint8_t>>>{
             {"int3", {0xcc}},
             {"int 3", {0xcd, 3}},
             {"prefixed int 3", {0x66, 0xcd, 3}},
             {"int1", {0xf1}}})
      Cases.push_back({Name,
                       Architecture,
                       InstructionMode::Default,
                       Bytes,
                       {0xb8, 42, 0, 0, 0, 0xc3},
                       true});
    for (const auto &[Name, Bytes] :
         std::vector<std::pair<const char *, std::vector<uint8_t>>>{
             {"ud0", {0x0f, 0xff}},
             {"ud1", {0x0f, 0xb9, 0xc0}},
             {"ud2", {0x0f, 0x0b}},
             {"fastfail", {0xcd, 0x29}}})
      Cases.push_back({Name,
                       Architecture,
                       InstructionMode::Default,
                       Bytes,
                       {0xb8, 42, 0, 0, 0, 0xc3},
                       false});
  }
  Cases.push_back({"thumb bkpt",
                   Arch::ARM,
                   InstructionMode::Thumb,
                   {1, 0xbe},
                   {42, 0x20, 0x70, 0x47},
                   true});
  Cases.push_back({"thumb udf",
                   Arch::ARM,
                   InstructionMode::Thumb,
                   {0xfe, 0xde},
                   {42, 0x20, 0x70, 0x47},
                   true});
  Cases.push_back({"arm bkpt",
                   Arch::ARM,
                   InstructionMode::ARM,
                   {0x71, 0, 0x20, 0xe1},
                   {42, 0, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1},
                   true});
  Cases.push_back({"arm udf",
                   Arch::ARM,
                   InstructionMode::ARM,
                   {0xfe, 0xde, 0xff, 0xe7},
                   {42, 0, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1},
                   true});
  for (const auto &[Name, Bytes] :
       std::vector<std::pair<const char *, std::vector<uint8_t>>>{
           {"a64 brk", {0x20, 0, 0x20, 0xd4}},
           {"a64 hlt", {0, 0, 0x40, 0xd4}},
           {"a64 udf", {0x34, 0x12, 0, 0}}})
    Cases.push_back({Name,
                     Arch::AArch64,
                     InstructionMode::Default,
                     Bytes,
                     {0x40, 0x05, 0x80, 0x52, 0xc0, 3, 0x5f, 0xd6},
                     false});
  return Cases;
}

BinaryImage trapImage(const TrapCase &Case) {
  auto Image = image(Case.Architecture);
  Image.Mode = Case.Mode;
  Image.Bits =
      Case.Architecture == Arch::X64 || Case.Architecture == Arch::AArch64
          ? Bitness::Bits64
          : Bitness::Bits32;
  auto &Text = Image.Segments.front();
  Text.Data = Case.Trap;
  Text.Data.insert(Text.Data.end(), Case.Continuation.begin(),
                   Case.Continuation.end());
  Text.Size = Text.Data.size();
  return Image;
}

TEST(ResumableTrapBoundary, SiblingTrapClassificationsAgreeWithLightDecode) {
  for (const auto &Case : siblingTraps())
    for (bool Detail : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << Case.Name << "/" << static_cast<int>(Case.Architecture)
                   << "/" << Detail);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Case.Architecture, Case.Mode));
      Dec.setDetail(Detail);
      DecodedInsn Insn;
      ASSERT_EQ(
          Dec.decodeOneLight(Case.Trap.data(), Case.Trap.size(), Entry, Insn),
          Case.Trap.size());
      // ARM control classification requires operand detail for predicates.
      // Trap identity itself is available from the opcode in either mode.
      if (Detail || Case.Architecture != Arch::ARM)
        EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
      EXPECT_EQ(Dec.isResumableTrap(Insn), Case.Resumable);
    }
}

TEST(ResumableTrapBoundary, SiblingTrapsRespectRangeEndsAndAdjacentEntries) {
  for (const auto &Case : siblingTraps())
    for (bool RangeEnd : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << Case.Name << "/" << static_cast<int>(Case.Architecture)
                   << "/" << RangeEnd);
      auto Image = trapImage(Case);
      const va_t End = Entry + Case.Trap.size();
      std::set<va_t> Entries;
      if (RangeEnd)
        Image.KnownCodeRanges.emplace_back(Entry, End);
      else
        Entries.insert(End);
      const auto Function = build(Image, Entries);
      EXPECT_TRUE(Function.hasCompleteLiftCoverage());
      if (Case.Resumable)
        EXPECT_FALSE(lowFunctionNeverReturns(Function, Case.Architecture));
      ASSERT_EQ(Function.Blocks.size(), 1u);
      const auto &Block = Function.Blocks.front();
      EXPECT_EQ(Block.EndAddr, End);
      EXPECT_TRUE(Block.Succs.empty());
      ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
      const auto &Boundary = Block.InstructionBoundaries.front();
      EXPECT_EQ(Boundary.Control, LowInstructionControl::Terminator);
      EXPECT_EQ(
          hasLowInstructionControlFlag(Boundary.ControlFlags,
                                       LowInstructionControlFlag::Resumable),
          Case.Resumable);
      for (const auto &Op : Block.Ops) {
        EXPECT_NE(Op.Opcode, NdOp::CALL);
        EXPECT_NE(Op.Opcode, NdOp::RETURN);
      }
    }
}

TEST(ResumableTrapBoundary,
     SiblingInteriorTrapsKeepOnlyResumableContinuations) {
  for (const auto &Case : siblingTraps()) {
    SCOPED_TRACE(::testing::Message()
                 << Case.Name << "/" << static_cast<int>(Case.Architecture));
    const auto Function = build(trapImage(Case));
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, Case.Resumable ? 3u : 1u);
    if (Case.Resumable)
      EXPECT_FALSE(lowFunctionNeverReturns(Function, Case.Architecture));
    ASSERT_EQ(Function.Blocks.size(), 1u);
    EXPECT_EQ(Function.Blocks.front().EndAddr,
              Entry + Case.Trap.size() +
                  (Case.Resumable ? Case.Continuation.size() : 0));
  }
}

TEST(ResumableTrapBoundary, ConditionalARMPathCanSkipTrap) {
  auto Image = image(Arch::ARM);
  Image.Mode = InstructionMode::Thumb;
  auto &Text = Image.Segments.front();
  Text.Data = {0,    0x28,  // cmp r0, #0
               0,    0xd1,  // bne Entry + 6
               0xfe, 0xde,  // udf #254
               42,   0x20,  // movs r0, #42
               0x70, 0x47}; // bx lr
  Text.Size = Text.Data.size();
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  DecodedInsn Insn;
  ASSERT_EQ(Dec.decodeOne(Text.Data.data(), Text.Data.size(), Entry, Insn), 2);
  ASSERT_EQ(Dec.decodeOne(Text.Data.data() + 2, Text.Data.size() - 2, Entry + 2,
                          Insn),
            2);
  EXPECT_FALSE(Dec.isFunctionTerminator(Insn));
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_FALSE(lowFunctionNeverReturns(Function, Arch::ARM));
  bool SawReturn = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      SawReturn |= Boundary.Control == LowInstructionControl::Return &&
                   Boundary.Address == Entry + 8;
  EXPECT_TRUE(SawReturn);
}

TEST(ResumableTrapBoundary, ThumbContinuationPeekDoesNotConsumeITState) {
  auto Image = image(Arch::ARM);
  Image.Mode = InstructionMode::Thumb;
  auto &Text = Image.Segments.front();
  Text.Data = {1,    0xbe,  // bkpt #1
               0x04, 0xbf,  // itt eq
               42,   0x20,  // moveq r0, #42
               7,    0x21,  // moveq r1, #7
               0x70, 0x47}; // bx lr
  Text.Size = Text.Data.size();
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 5u);
  unsigned ConditionalWrites = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      ConditionalWrites += Op.Opcode == NdOp::SELECT && Op.Output.isReg() &&
                           (Op.Addr == Entry + 4 || Op.Addr == Entry + 6);
  EXPECT_EQ(ConditionalWrites, 2u);
}

TEST(ResumableTrapBoundary, OrdinaryInterruptKeepsItsContinuation) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    const TrapCase Case{"int 0x80",
                        Architecture,
                        InstructionMode::Default,
                        {0xcd, 0x80},
                        {0xb8, 42, 0, 0, 0, 0xc3},
                        true};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Architecture));
    DecodedInsn Insn;
    ASSERT_EQ(Dec.decodeOne(Case.Trap.data(), Case.Trap.size(), Entry, Insn),
              2);
    EXPECT_FALSE(Dec.isFunctionTerminator(Insn));
    EXPECT_FALSE(Dec.isResumableTrap(Insn));
    const auto Function = build(trapImage(Case));
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, 3u);
    EXPECT_FALSE(lowFunctionNeverReturns(Function, Architecture));
  }
}

} // namespace

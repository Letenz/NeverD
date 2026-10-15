//===- RegistrationFilterStateTests.cpp - Correlated SEH filter effects ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks search dispositions, written states and suspended finally frames.

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

namespace {
enum class Variant {
  Correlated,
  SameState,
  BothContinue,
  CapturedCursor,
  StoresAroundRaise,
  ReversedEdges,
  NegativeDisposition,
  SkipFilterAfterCallback,
  SelectedHandler,
  UnknownDisposition,
  EscapingSearch,
  OuterHandler,
  BrokenStack,
  RecurrentArm,
  WrongStoreWidth,
  IncompleteCallInstruction,
};

void addReturn(LowBlock &Block, va_t Address) {
  const auto First = Block.Ops.size();
  emitOp(Block, Address, NdOp::RETURN, {}, {});
  Block.EndAddr = Address + 1;
  Block.InstructionBoundaries.push_back({Address, 1, First, 1});
  auto &Boundary = Block.InstructionBoundaries.back();
  Boundary.Control = LowInstructionControl::Return;
  Boundary.ControlFlags = LowInstructionControlFlag::Return;
}

void addStateStore(LowBlock &Block, va_t Address, int32_t Level,
                   RegistrationChainInfo &Chain) {
  const auto First = Block.Ops.size();
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-4), 4)});
  emitOp(Block, Address, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::cst(uint32_t(Level), 4)});
  Block.InstructionBoundaries.push_back({Address, 7, First, 2});
  Chain.TryLevelStores.push_back({Address, Address + 7, Level});
}

LowFunc filterFrame(Variant V) {
  auto F = makeJumpFrame();
  F.Blocks.resize(V == Variant::StoresAroundRaise ? 18 : 16);
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes = {{-1, 0x1880, 0x1120, false},
                  {0, 0x1800, 0x1120, false},
                  {1, 0, 0x1840, true}};
  F.Blocks[3].Succs = {8, 5};
  auto &Active = F.Blocks[8];
  Active.Id = 8;
  Active.StartAddr = 0x1140;
  Active.EndAddr = 0x1147;
  Active.Succs = {4};
  addStateStore(Active, 0x1140, 2, Chain);
  for (auto &Op : F.Blocks[4].Ops)
    Op.Addr += 0x41;
  for (auto &Boundary : F.Blocks[4].InstructionBoundaries)
    Boundary.Address += 0x41;
  F.Blocks[4].StartAddr += 0x41;
  F.Blocks[4].EndAddr += 0x41;

  auto &Filter = F.Blocks[7];
  Filter.Id = 7;
  Filter.StartAddr = 0x1800;
  Filter.EndAddr = 0x1802;
  Filter.Succs = V == Variant::ReversedEdges ? std::vector<int>{12, 11}
                                             : std::vector<int>{11, 12};
  emitOp(Filter, 0x1800, NdOp::COND_BR, {},
         {NdVar::cst(0x1940, 8), NdVar::reg(x86reg::RCX, 4)});
  Filter.InstructionBoundaries = {{0x1800, 2, 0, 1}};
  Filter.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Filter.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch |
      LowInstructionControlFlag::Conditional;
  for (unsigned I = 0; I != 2; ++I) {
    auto &Arm = F.Blocks[11 + I];
    Arm.Id = 11 + I;
    Arm.StartAddr = I ? 0x1940 : 0x1900;
    Arm.EndAddr = Arm.StartAddr + 7;
    Arm.Succs = {int(14 + I)};
    const int32_t Level = V == Variant::SameState              ? 0
                          : V == Variant::CapturedCursor || !I ? -1
                                                               : 0;
    addStateStore(Arm, Arm.StartAddr, Level, Chain);
    const uint32_t Disposition = !I && V == Variant::NegativeDisposition
                                     ? 0x80000000u
                                 : !I || V == Variant::BothContinue ? UINT32_MAX
                                 : V == Variant::SelectedHandler    ? 1
                                                                    : 0;
    auto &DispositionBlock = F.Blocks[14 + I];
    DispositionBlock.Id = 14 + I;
    DispositionBlock.StartAddr = Arm.EndAddr;
    DispositionBlock.EndAddr = Arm.EndAddr + 5;
    DispositionBlock.Succs = {13};
    emitOp(
        DispositionBlock, Arm.EndAddr, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
        {I && V == Variant::UnknownDisposition ? NdVar::reg(x86reg::RCX, 4)
                                               : NdVar::cst(Disposition, 4)});
    DispositionBlock.InstructionBoundaries = {{Arm.EndAddr, 5, 0, 1}};
  }
  if (V == Variant::RecurrentArm)
    F.Blocks[12].Succs = {12};
  if (V == Variant::WrongStoreWidth)
    F.Blocks[12].Ops[1].Inputs[1].Size = 2;
  auto &Return = F.Blocks[13];
  Return.Id = 13;
  Return.StartAddr = 0x1980;
  if (V == Variant::BrokenStack) {
    emitOp(Return, 0x1980, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    Return.InstructionBoundaries = {{0x1980, 3, 0, 1}};
  }
  addReturn(Return, V == Variant::BrokenStack ? 0x1983 : 0x1980);

  auto &Outer = F.Blocks[10];
  Outer.Id = 10;
  Outer.StartAddr = 0x1880;
  emitOp(Outer, 0x1880, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(V == Variant::NegativeDisposition ? UINT32_MAX - 1
                     : V == Variant::EscapingSearch    ? 0
                     : V == Variant::OuterHandler      ? 1
                                                       : UINT32_MAX,
                     4)});
  Outer.InstructionBoundaries = {{0x1880, 5, 0, 1}};
  addReturn(Outer, 0x1885);

  auto &FinallyEntry = F.Blocks[9];
  FinallyEntry.Id = 9;
  FinallyEntry.StartAddr = 0x1840;
  auto &Finally = V == Variant::StoresAroundRaise ? F.Blocks[16] : FinallyEntry;
  if (V == Variant::StoresAroundRaise) {
    addStateStore(FinallyEntry, 0x1840, 1, Chain);
    FinallyEntry.EndAddr = 0x1847;
    FinallyEntry.Succs = {16};
    Finally.Id = 16;
    Finally.StartAddr = 0x1847;
  }
  const va_t Arguments = Finally.StartAddr;
  const size_t First = Finally.Ops.size();
  emitOp(Finally, Arguments, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  for (unsigned I = 0; I != 4; ++I) {
    emitOp(Finally, Arguments, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(I * 4, 4)});
    emitOp(Finally, Arguments, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
  }
  Finally.InstructionBoundaries.push_back(
      {Arguments, 16, First, Finally.Ops.size() - First});
  const auto Call = Finally.Ops.size();
  emitOp(Finally, Arguments + 16, NdOp::INDIR_CALL, {},
         {NdVar::cst(0x4100, 4)});
  Finally.InstructionBoundaries.push_back({Arguments + 16, 6, Call, 1});
  Finally.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  Finally.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Call;
  if (V == Variant::IncompleteCallInstruction) {
    emitOp(Finally, Arguments + 16, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {NdVar::cst(0, 4)});
    ++Finally.InstructionBoundaries.back().OpCount;
  }
  va_t End = Arguments + 22;
  if (V == Variant::StoresAroundRaise) {
    addStateStore(Finally, End, 0, Chain);
    End += 7;
    Finally.EndAddr = End;
    Finally.Succs = {17};
    auto &EndBlock = F.Blocks[17];
    EndBlock.Id = 17;
    EndBlock.StartAddr = End;
    addReturn(EndBlock, End);
  } else {
    addReturn(Finally, End);
  }
  if (V != Variant::SkipFilterAfterCallback) {
    LowBlock InitialState;
    InitialState.Id = F.Blocks.size();
    InitialState.StartAddr = 0x1088;
    InitialState.EndAddr = 0x108f;
    InitialState.Succs = {2};
    F.Blocks[1].Succs = {InitialState.Id};
    addStateStore(InitialState, 0x1088, 0, Chain);
    F.Blocks.push_back(std::move(InitialState));
  }
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  return F;
}

RegistrationStateAnalysis analyze(Variant V) {
  auto F = filterFrame(V);
  EH3JumpRuntime Runtime;
  Runtime.ImportSlots.insert(0x4100);
  Runtime.Capture = [V](const LowOp &Op, const FrameState &Frame, bool,
                        size_t &) -> std::optional<EH3JumpCapture> {
    if (Op.Addr != 0x1100)
      return std::nullopt;
    EH3JumpCapture Capture;
    Capture.Call = Op.Addr;
    Capture.ReturnPC = 0x1105;
    Capture.BufferOffset = -144;
    Capture.StackOffset = -192;
    Capture.UnwindTarget = 0x3300;
    Capture.Level = V == Variant::SkipFilterAfterCallback ? -1 : 0;
    constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                      x86reg::RSI};
    for (unsigned I = 0; I != 4; ++I)
      Capture.Nonvolatile[I] =
          Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
    return Capture;
  };
  Runtime.Resume = [](const LowOp &Op, size_t &) { return Op.Addr == 0x115b; };
  Runtime.Raise = [](const LowOp &Op, const FrameState &, size_t &) {
    return Op.Addr == 0x1850 || Op.Addr == 0x1857;
  };
  RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, &Runtime);
  return Solver.run();
}
} // namespace

TEST(RegistrationFilterState,
     PreservesDispositionAndWrittenStateAtSharedReturn) {
  for (auto V : {Variant::Correlated, Variant::SameState, Variant::BothContinue,
                 Variant::CapturedCursor, Variant::StoresAroundRaise,
                 Variant::ReversedEdges, Variant::NegativeDisposition}) {
    SCOPED_TRACE(unsigned(V));
    const auto Result = analyze(V);
    for (const auto &Diagnostic : Result.Diagnostics)
      ADD_FAILURE() << Diagnostic;
    EXPECT_TRUE(Result.Complete);
    EXPECT_TRUE(Result.SEHStateStoresComplete);
    EXPECT_TRUE(Result.SEHJumpResumptionsComplete);
    EXPECT_EQ(Result.SEHJumpResumptions.size(), 1u);
    if (!Result.SEHJumpResumptions.empty()) {
      const auto &Resume = Result.SEHJumpResumptions.front();
      EXPECT_TRUE(Resume.StateInFrame);
      const std::vector<int32_t> Expected =
          V == Variant::SameState || V == Variant::StoresAroundRaise
              ? std::vector<int32_t>{0}
          : V == Variant::CapturedCursor ? std::vector<int32_t>{-1}
                                         : std::vector<int32_t>{-1, 0};
      EXPECT_EQ(Resume.ResumedLevels, Expected);
    }
  }
}

TEST(RegistrationFilterState, SkippingAnotherScopeCannotReuseTheCallbackState) {
  const auto Result = analyze(Variant::SkipFilterAfterCallback);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
  const auto &Resume = Result.SEHJumpResumptions.front();
  EXPECT_EQ(Resume.ResumedLevels, (std::vector<int32_t>{-1}));
  EXPECT_FALSE(Resume.StateInFrame);
}

TEST(RegistrationFilterState,
     EveryFilterPathMustRestoreAndResumeTheInvocation) {
  for (auto V :
       {Variant::SelectedHandler, Variant::UnknownDisposition,
        Variant::EscapingSearch, Variant::OuterHandler, Variant::BrokenStack,
        Variant::RecurrentArm, Variant::WrongStoreWidth,
        Variant::IncompleteCallInstruction}) {
    SCOPED_TRACE(unsigned(V));
    const auto Result = analyze(V);
    EXPECT_FALSE(Result.Complete);
    EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
    EXPECT_TRUE(Result.SEHJumpResumptions.empty());
  }
}

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

#include "llvm/ADT/STLExtras.h"

#include <utility>

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
  SkipFilterBeforeResume,
  SkipTwoFiltersBeforeResume,
  TwoResumeMappings,
  SkipFilterAfterCallback,
  SelectedHandler,
  HandlerOnly,
  HandlerCleanup,
  RecurrentSelection,
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

LowFunc filterFrame(Variant V, unsigned Protocol = 0) {
  auto F = makeJumpFrame();
  F.Blocks.resize(V == Variant::StoresAroundRaise ? 18 : 16);
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes = {{-1, 0x1880, 0x1120, false},
                  {0, 0x1800, 0x1120, false},
                  {1, 0, 0x1840, true}};
  if (V == Variant::SkipTwoFiltersBeforeResume)
    Chain.Scopes.push_back({1, 0x1800, 0x1120, false});
  if (V == Variant::TwoResumeMappings)
    Chain.Scopes = {{-1, 0x1880, 0x1120, false},
                    {0, 0x1880, 0x1120, false},
                    {1, 0x1880, 0x1120, false},
                    {0, 0x1800, 0x1120, false},
                    {3, 0, 0x1840, true}};
  F.Blocks[3].Succs = {8, 5};
  auto &Active = F.Blocks[8];
  Active.Id = 8;
  Active.StartAddr = 0x1140;
  Active.EndAddr = 0x1147;
  Active.Succs = {4};
  addStateStore(Active, 0x1140, V == Variant::TwoResumeMappings ? 4 : 2, Chain);
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
    const int32_t Level = V == Variant::TwoResumeMappings        ? (I ? 3 : 2)
                          : V == Variant::SameState              ? 0
                          : V == Variant::HandlerCleanup         ? 3
                          : V == Variant::RecurrentSelection     ? 2
                          : V == Variant::CapturedCursor || !I   ? -1
                          : V == Variant::SkipFilterBeforeResume ? 1
                          : V == Variant::SkipTwoFiltersBeforeResume ? 3
                                                                     : 0;
    addStateStore(Arm, Arm.StartAddr, Level, Chain);
    const uint32_t Disposition =
        V == Variant::HandlerOnly || V == Variant::HandlerCleanup ||
                V == Variant::RecurrentSelection
            ? 1
        : !I && V == Variant::NegativeDisposition ? 0x80000000u
        : !I || V == Variant::BothContinue        ? UINT32_MAX
        : V == Variant::SelectedHandler           ? 1
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
      LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
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
    addStateStore(InitialState, 0x1088, V == Variant::TwoResumeMappings ? 1 : 0,
                  Chain);
    F.Blocks.push_back(std::move(InitialState));
  }
  if (V == Variant::HandlerCleanup) {
    LowBlock Cleanup;
    Cleanup.Id = F.Blocks.size();
    Cleanup.StartAddr = 0x1a00;
    addReturn(Cleanup, 0x1a00);
    F.Blocks.push_back(std::move(Cleanup));
    Chain.Scopes.push_back({1, 0, 0x1a00, true});
  }
  if (Protocol) {
    initializeEH4JumpFrame(F, Protocol == 2);
    for (auto &Store : Chain.TryLevelStores)
      if (Store.Level == -1) {
        Store.Level = -2;
        for (auto &Block : F.Blocks)
          for (auto &Op : Block.Ops)
            if (Op.Addr == Store.StoreVA && Op.Opcode == NdOp::STORE)
              Op.Inputs[1] = NdVar::cst(uint32_t(-2), 4);
      }
  }
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  return F;
}

RegistrationStateAnalysis analyze(LowFunc F, Variant V, va_t CookieVA = 0x4000,
                                  bool UnwindJump = false) {
  const bool EH4 =
      F.ExceptionMetadata->Personality == ExceptionPersonality::ExceptHandler4;
  SEHJumpRuntime Runtime;
  Runtime.ImportSlots.insert(0x4100);
  Runtime.Capture = [V, EH4](const LowOp &Op, const FrameState &Frame, bool,
                             size_t &) -> std::optional<SEHJumpCapture> {
    if (Op.Addr != 0x1100)
      return std::nullopt;
    SEHJumpCapture Capture;
    Capture.Call = Op.Addr;
    Capture.ReturnPC = 0x1105;
    Capture.BufferOffset = -144;
    Capture.StackOffset = -192;
    Capture.UnwindTarget = 0x3300;
    Capture.Level = V == Variant::SkipFilterAfterCallback ? (EH4 ? -2 : -1)
                    : V == Variant::TwoResumeMappings     ? 1
                                                          : 0;
    if (EH4) {
      Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH4;
      Capture.SecurityCookieVA = 0x4000;
    }
    constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                      x86reg::RSI};
    for (unsigned I = 0; I != 4; ++I)
      Capture.Nonvolatile[I] =
          Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
    return Capture;
  };
  Runtime.Resume = [](const LowOp &Op, size_t &) {
    return Op.Addr == 0x115b || Op.Addr == 0x1a0b;
  };
  Runtime.Raise = [](const LowOp &Op, const FrameState &, size_t &) {
    return Op.Addr == 0x1850 || Op.Addr == 0x1857;
  };
  std::vector<RegistrationSEHControlContract> Controls;
  if (UnwindJump)
    for (const auto &Block : F.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::RETURN)
          Controls.push_back({RegistrationSEHControlContract::Kind::NearReturn,
                              Op.Addr, Block.EndAddr, 0});
  RegistrationStateSolver Solver(
      F, EH4 ? CookieVA : 0, 0, nullptr, nullptr, nullptr, nullptr,
      UnwindJump ? &Controls : nullptr, nullptr, nullptr, &Runtime);
  return Solver.run();
}

RegistrationStateAnalysis analyze(Variant V, unsigned Protocol = 0) {
  return analyze(filterFrame(V, Protocol), V);
}

LowFunc callbackActivatedFrame() {
  auto F = filterFrame(Variant::TwoResumeMappings);
  auto &Chain = *F.ExceptionMetadata->Registration;
  // State 2 is entered only by the search filter. Give its roots distinct
  // addresses so another scope cannot accidentally provide their proof.
  Chain.Scopes[2].FilterVA = 0x19c0;
  Chain.Scopes[2].HandlerVA = 0x19d0;
  LowBlock Filter;
  Filter.Id = F.Blocks.size();
  Filter.StartAddr = 0x19c0;
  emitOp(Filter, 0x19c0, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(UINT32_MAX, 4)});
  Filter.InstructionBoundaries = {{0x19c0, 5, 0, 1}};
  addReturn(Filter, 0x19c5);
  F.Blocks.push_back(std::move(Filter));
  LowBlock Handler;
  Handler.Id = F.Blocks.size();
  Handler.StartAddr = 0x19d0;
  Handler.EndAddr = 0x19d7;
  Handler.Succs = {6};
  addStateStore(Handler, 0x19d0, -1, Chain);
  F.Blocks.push_back(std::move(Handler));
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  return F;
}
} // namespace

TEST(RegistrationFilterState, CallbackActivatedScopeHasIndependentRoots) {
  const auto Result =
      analyze(callbackActivatedFrame(), Variant::TwoResumeMappings);
  ASSERT_TRUE(Result.Complete);
  ASSERT_GE(Result.Blocks.size(), 2u);
  const auto &Filter = Result.Blocks[Result.Blocks.size() - 2];
  const auto &Handler = Result.Blocks.back();
  EXPECT_TRUE(Filter.Reached);
  EXPECT_TRUE(Filter.CallbackOnly);
  EXPECT_EQ(Filter.Levels, (std::vector<int32_t>{2}));
  EXPECT_TRUE(Handler.Reached);
  EXPECT_FALSE(Handler.CallbackOnly);
  EXPECT_EQ(Handler.Levels, (std::vector<int32_t>{1}));
}

TEST(RegistrationFilterState, UnknownSearchCannotPublishCompleteRootProofs) {
  auto F = callbackActivatedFrame();
  F.Blocks[15].Ops.front().Inputs[0] = NdVar::reg(x86reg::RCX, 4);
  const auto Result = analyze(std::move(F), Variant::TwoResumeMappings);
  EXPECT_FALSE(Result.Complete);
  ASSERT_GE(Result.Blocks.size(), 2u);
  const auto &Filter = Result.Blocks[Result.Blocks.size() - 2];
  const auto &Handler = Result.Blocks.back();
  EXPECT_TRUE(!Filter.Reached || Filter.Unknown);
  EXPECT_TRUE(!Handler.Reached || Handler.Unknown);
}

TEST(RegistrationFilterState,
     PreservesDispositionAndWrittenStateAtSharedReturn) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (auto V :
         {Variant::Correlated, Variant::SameState, Variant::BothContinue,
          Variant::CapturedCursor, Variant::StoresAroundRaise,
          Variant::ReversedEdges, Variant::NegativeDisposition,
          Variant::SkipFilterBeforeResume, Variant::SkipTwoFiltersBeforeResume,
          Variant::TwoResumeMappings}) {
      SCOPED_TRACE(unsigned(V));
      SCOPED_TRACE(Protocol);
      const auto Result = analyze(V, Protocol);
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(Result.Complete);
      EXPECT_TRUE(Result.SEHStateStoresComplete);
      EXPECT_TRUE(Result.SEHJumpResumptionsComplete);
      EXPECT_EQ(Result.SEHJumpResumptions.size(), 1u);
      if (!Result.SEHJumpResumptions.empty()) {
        const auto &Resume = Result.SEHJumpResumptions.front();
        const int32_t Outside = Protocol ? -2 : -1;
        const std::vector<int32_t> Expected =
            V == Variant::SameState || V == Variant::StoresAroundRaise
                ? std::vector<int32_t>{0}
            : V == Variant::TwoResumeMappings ? std::vector<int32_t>{0, 1}
            : V == Variant::CapturedCursor ? std::vector<int32_t>{Outside}
                                           : std::vector<int32_t>{Outside, 0};
        EXPECT_EQ(Resume.ResumedLevels, Expected);
        std::vector<RegistrationSEHJumpState> Restores;
        for (int32_t Level : Expected) {
          const int32_t Frame =
              Level == 0 && V == Variant::SkipFilterBeforeResume       ? 1
              : Level == 0 && V == Variant::SkipTwoFiltersBeforeResume ? 3
                                                                       : Level;
          Restores.push_back(
              {Frame, Level, V == Variant::TwoResumeMappings ? 1 : 0});
        }
        if (V == Variant::TwoResumeMappings)
          Restores = {{2, 1, 1}, {3, 0, 1}};
        EXPECT_EQ(Resume.StateRestores, Restores);
      }
    }
}

TEST(RegistrationFilterState, SkippingAnotherScopeCannotReuseTheCallbackState) {
  const auto Result = analyze(Variant::SkipFilterAfterCallback);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
  const auto &Resume = Result.SEHJumpResumptions.front();
  EXPECT_EQ(Resume.ResumedLevels, (std::vector<int32_t>{-1}));
  EXPECT_EQ(Resume.StateRestores,
            (std::vector<RegistrationSEHJumpState>{{-1, -1}, {0, -1}}));
}

TEST(RegistrationFilterState, EveryFilterPathMustCloseTheInvocation) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (auto V :
         {Variant::RecurrentSelection, Variant::UnknownDisposition,
          Variant::EscapingSearch, Variant::BrokenStack, Variant::RecurrentArm,
          Variant::WrongStoreWidth, Variant::IncompleteCallInstruction}) {
      SCOPED_TRACE(unsigned(V));
      SCOPED_TRACE(Protocol);
      const auto Result = analyze(V, Protocol);
      EXPECT_FALSE(Result.Complete);
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
      EXPECT_TRUE(Result.SEHJumpResumptions.empty());
    }
}

TEST(RegistrationFilterState, EH4SearchKeepsBothCookiesIndependentOfState) {
  for (unsigned Protocol : {1u, 2u}) {
    SCOPED_TRACE(Protocol);
    for (va_t Cookie : {va_t(0), va_t(0x4004)}) {
      const auto Result = analyze(filterFrame(Variant::Correlated, Protocol),
                                  Variant::Correlated, Cookie);
      EXPECT_FALSE(Result.SecurityCookiesComplete);
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
    }
    for (int32_t Offset : {-40, -28, -29, -32, -33}) {
      SCOPED_TRACE(Offset);
      auto F = filterFrame(Variant::Correlated, Protocol);
      LowBlock Store;
      Store.Id = F.Blocks.size();
      Store.StartAddr = 0x1a00;
      Store.EndAddr = 0x1a07;
      Store.Succs = {7};
      emitOp(Store, 0x1a00, NdOp::INT_ADD, NdVar::tmp(0, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
      emitOp(Store, 0x1a00, NdOp::STORE, {},
             {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
      Store.InstructionBoundaries = {{0x1a00, 7, 0, 2}};
      F.ExceptionMetadata->Registration->Scopes[1].FilterVA = 0x1a00;
      F.Blocks.push_back(std::move(Store));
      const auto Result = analyze(std::move(F), Variant::Correlated);
      const bool Valid = Offset == -40 || (Protocol == 1 && Offset <= -32);
      EXPECT_EQ(Result.Complete, Valid);
      EXPECT_EQ(Result.SEHJumpResumptionsComplete, Valid);
      EXPECT_EQ(Result.SEHJumpResumptions.empty(), !Valid);
    }
  }
}

TEST(RegistrationFilterState, HandlerSelectionAbandonsOnlyItsSuspendedPath) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (auto V : {Variant::SelectedHandler, Variant::OuterHandler,
                   Variant::HandlerOnly, Variant::HandlerCleanup}) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(unsigned(V));
      const auto Result = analyze(V, Protocol);
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      ASSERT_TRUE(Result.Complete);
      ASSERT_TRUE(Result.SEHJumpResumptionsComplete);
      ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
      const auto &Resume = Result.SEHJumpResumptions.front();
      EXPECT_EQ(Resume.SelectedScopes,
                (std::vector<uint32_t>{V == Variant::OuterHandler ? 0u : 1u}));
      const bool HandlerOnly =
          V == Variant::HandlerOnly || V == Variant::HandlerCleanup;
      const int32_t Outside = Protocol ? -2 : -1;
      EXPECT_EQ(Resume.ResumedLevels, HandlerOnly
                                          ? std::vector<int32_t>{}
                                          : std::vector<int32_t>{Outside});
      const std::vector<RegistrationSEHJumpState> Restored =
          HandlerOnly
              ? std::vector<RegistrationSEHJumpState>{}
              : std::vector<RegistrationSEHJumpState>{{Outside, Outside, 0}};
      EXPECT_EQ(Resume.StateRestores, Restored);
      if (V == Variant::HandlerCleanup) {
        const auto &Cleanup = Result.Blocks[Result.Blocks.size() - 1];
        EXPECT_TRUE(Cleanup.Reached);
        EXPECT_TRUE(Cleanup.CallbackOnly);
      }
    }
}

TEST(RegistrationFilterState,
     SelectedHandlerRequiresACompleteRaiseInstruction) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol) {
    SCOPED_TRACE(Protocol);
    auto F = filterFrame(Variant::HandlerOnly, Protocol);
    auto &Block = F.Blocks[9];
    const auto At = Block.Ops.end() - 1;
    LowOp Extra;
    Extra.Addr = 0x1850;
    Extra.Opcode = NdOp::COPY;
    Extra.Output = NdVar::reg(x86reg::RAX, 4);
    Extra.NumInputs = 1;
    Extra.Inputs[0] = NdVar::cst(0, 4);
    Block.Ops.insert(At, Extra);
    ++Block.InstructionBoundaries[1].OpCount;
    ++Block.InstructionBoundaries[2].FirstOp;
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
    const auto Result = analyze(std::move(F), Variant::HandlerOnly);
    EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
    EXPECT_TRUE(Result.SEHJumpResumptions.empty());
  }
}

TEST(RegistrationFilterState, CleanupJumpRetiresThePendingHandler) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(Mutation);
      auto F = filterFrame(Variant::HandlerCleanup, Protocol);
      if (Mutation == 7)
        F.ExceptionMetadata->CodeRange.End = 0x1a00;
      if (Mutation == 8)
        F.ExceptionMetadata->CodeRange.End = 0x115b;
      auto Cleanup = F.Blocks[4];
      Cleanup.Id = F.Blocks.back().Id;
      const va_t Delta = 0x1a00 - Cleanup.StartAddr;
      Cleanup.StartAddr += Delta;
      Cleanup.EndAddr += Delta;
      Cleanup.Succs.clear();
      for (auto &Op : Cleanup.Ops)
        Op.Addr += Delta;
      for (auto &Boundary : Cleanup.InstructionBoundaries)
        Boundary.Address += Delta;
      if (Mutation == 1 || Mutation == 2)
        Cleanup.Ops[4].Inputs[1] = NdVar::cst(Mutation, 4);
      if (Mutation == 3)
        Cleanup.Ops[1].Inputs[1] = NdVar::cst(uint32_t(-140), 4);
      if (Mutation == 4) {
        Cleanup.Ops[4].Inputs[0] = NdVar::reg(x86reg::RBP, 4);
      } else if (Mutation == 5) {
        emitOp(Cleanup, 0x1a0b, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
               {NdVar::cst(0, 4)});
        ++Cleanup.InstructionBoundaries.back().OpCount;
      } else if (Mutation == 6) {
        Cleanup.InstructionBoundaries.back().ControlFlags =
            LowInstructionControlFlag::Call;
      }
      for (unsigned I = 0; I != Cleanup.Ops.size(); ++I)
        Cleanup.Ops[I].Seq = I;
      F.Blocks.back() = std::move(Cleanup);
      const auto Result =
          analyze(std::move(F), Variant::HandlerCleanup, 0x4000, true);
      if (Mutation >= 3 && Mutation != 7) {
        EXPECT_FALSE(Result.Complete);
        EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
        continue;
      }
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      ASSERT_TRUE(Result.Complete);
      ASSERT_TRUE(Result.SEHJumpResumptionsComplete);
      const auto Outer =
          llvm::find_if(Result.SEHJumpResumptions, [](const auto &Resume) {
            return Resume.Address == 0x115b;
          });
      ASSERT_NE(Outer, Result.SEHJumpResumptions.end());
      EXPECT_TRUE(Outer->SelectedScopes.empty());
      EXPECT_EQ(Outer->ResumedLevels, (std::vector<int32_t>{0}));
      EXPECT_EQ(Outer->StateRestores,
                (std::vector<RegistrationSEHJumpState>{{1, 0, 0}}));
    }
}

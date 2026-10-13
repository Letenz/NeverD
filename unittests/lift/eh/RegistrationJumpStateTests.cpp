//===- RegistrationJumpStateTests.cpp - EH3 buffer lifetime and resumption ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

namespace {
LowFunc jumpFrame(unsigned Mutation) {
  auto F = makeBranchingFrame();
  F.Blocks.resize(7);
  F.Blocks[0].Succs = {1};
  for (unsigned I = 1; I != F.Blocks.size(); ++I) {
    F.Blocks[I] = {};
    F.Blocks[I].Id = I;
  }
  auto &Setup = F.Blocks[1];
  Setup.StartAddr = 0x1080;
  Setup.EndAddr = 0x1088;
  Setup.Succs = {2};
  emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(160, 4)});
  emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Setup, 0x1083, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::reg(x86reg::RSP, 4)});
  emitOp(Setup, 0x1086, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  Setup.InstructionBoundaries = {
      {0x1080, 3, 0, 1}, {0x1083, 3, 1, 2}, {0x1086, 2, 3, 1}};
  auto &Capture = F.Blocks[2];
  Capture.StartAddr = 0x1100;
  Capture.EndAddr = 0x1105;
  Capture.Succs = {3};
  emitOp(Capture, 0x1100, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x3000, 4)});
  Capture.InstructionBoundaries = {{0x1100, 5, 0, 1}};
  Capture.InstructionBoundaries[0].Control = LowInstructionControl::Call;
  Capture.InstructionBoundaries[0].ControlFlags =
      LowInstructionControlFlag::Call;
  Capture.InstructionBoundaries[0].Immediate = 0x3000;
  auto &Resume = F.Blocks[3];
  Resume.StartAddr = 0x1105;
  Resume.EndAddr = 0x110f;
  Resume.Succs = {4, 5};
  emitOp(Resume, 0x1105, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4),
          NdVar::cst(uint32_t(Mutation == 2 ? -104 : -143), 4)});
  if (Mutation == 1 || Mutation == 2)
    emitOp(Resume, 0x1105, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(0, 1)});
  if (Mutation == 3)
    emitOp(Resume, 0x1105, NdOp::CALL, {}, {NdVar::cst(0x3200, 4)});
  if (Mutation == 6)
    emitOp(Resume, 0x1105, NdOp::STORE, {},
           {NdVar::cst(0x4101, 4), NdVar::cst(0, 1)});
  emitOp(Resume, 0x1108, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  emitOp(Resume, 0x110b, NdOp::COND_BR, {},
         {NdVar::cst(0x1120, 8), NdVar::reg(x86reg::RAX, 4)});
  Resume.InstructionBoundaries = {
      {0x1105, 3, 0, uint32_t(Resume.Ops.size() - 2)},
      {0x1108, 3, uint32_t(Resume.Ops.size() - 2), 1},
      {0x110b, 4, uint32_t(Resume.Ops.size() - 1), 1}};
  Resume.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Resume.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch |
      LowInstructionControlFlag::Conditional;
  auto &Jump = F.Blocks[4];
  Jump.StartAddr = 0x110f;
  Jump.EndAddr = 0x111f;
  emitOp(Jump, 0x110f, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
  emitOp(Jump, 0x1112, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-144), 4)});
  emitOp(Jump, 0x1112, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(0, 4)});
  emitOp(Jump, 0x1115, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  emitOp(Jump, 0x1115, NdOp::STORE, {}, {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
  emitOp(Jump, 0x111a, NdOp::CALL, {}, {NdVar::cst(0x3100, 4)});
  Jump.InstructionBoundaries = {{0x110f, 3, 0, 1},
                                {0x1112, 3, 1, 2},
                                {0x1115, 5, 3, 2},
                                {0x111a, 5, 5, 1}};
  Jump.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  Jump.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Call | LowInstructionControlFlag::NoReturn;
  Jump.InstructionBoundaries.back().Immediate = 0x3100;
  auto &Retire = F.Blocks[5];
  Retire.StartAddr = 0x1120;
  Retire.EndAddr = 0x1127;
  Retire.Succs = {6};
  addSlotStore(Retire, -1);
  Retire.InstructionBoundaries = {{0x1120, 7, 0, 2}};
  F.ExceptionMetadata->Registration->TryLevelStores = {{0x1120, 0x1127, -1}};
  auto &Return = F.Blocks[6];
  Return.StartAddr = 0x1130;
  Return.EndAddr = 0x1134;
  emitOp(Return, 0x1130, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Return, 0x1130, NdOp::LOAD, NdVar::tmp(1, 4), {NdVar::tmp(0, 4)});
  emitOp(Return, 0x1130, NdOp::STORE, {}, {NdVar::cst(0, 4), NdVar::tmp(1, 4)},
         NdMemoryAddressSpace::X86FS);
  Return.InstructionBoundaries = {{0x1130, 4, 0, 3}};
  return F;
}
} // namespace

TEST(RegistrationJumpState, CaptureSurvivesOnlyCheckedStorageAndCalls) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto F = jumpFrame(Mutation);
    EH3JumpRuntime Runtime;
    Runtime.ImportSlots.insert(0x4100);
    Runtime.Capture = [&](const LowOp &Op, const FrameState &Frame,
                          bool Installed,
                          size_t &) -> std::optional<EH3JumpCapture> {
      if (Op.Addr != 0x1100 || !Installed || Mutation == 4)
        return std::nullopt;
      EXPECT_EQ(Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset,
                -192);
      EH3JumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = Mutation == 5 ? 0x1106 : 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x111a;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    if (Mutation == 0) {
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(Result.Complete);
      EXPECT_TRUE(Result.RegistrationLifetimeComplete);
      EXPECT_TRUE(Result.HasEH3JumpCaptures);
      EXPECT_TRUE(Result.SEHJumpResumptionsComplete);
      ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
      const auto &Resume = Result.SEHJumpResumptions.front();
      EXPECT_EQ(Resume.Address, 0x111au);
      EXPECT_EQ(Resume.CaptureCall, 0x1100u);
      EXPECT_EQ(Resume.TargetVA, 0x1105u);
      EXPECT_EQ(Resume.StackOffset, -192);
    } else {
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
      EXPECT_TRUE(Result.SEHJumpResumptions.empty());
    }
  }
}

TEST(RegistrationJumpState, CaptureWritesOnlyTheSpecifiedRuntimeFields) {
  EH3JumpCapture Capture;
  Capture.BufferOffset = -144;
  Capture.StackOffset = -192;
  Capture.ReturnPC = 0x1105;
  Capture.UnwindTarget = 0x3300;
  Capture.Nonvolatile[0] = FrameValue::frame(0);
  for (unsigned I = 1; I != Capture.Nonvolatile.size(); ++I)
    Capture.Nonvolatile[I] = FrameValue::constant(0x11223344 + I);
  for (unsigned Mutation = 0; Mutation != 42; ++Mutation) {
    FrameState Frame;
    Frame.store(-104, 4, FrameValue::constant(7));
    storeEH3JumpCapture(Frame, Capture);
    EXPECT_EQ(Frame.load(-104, 4).Constant, 7u);
    EXPECT_TRUE(hasEH3JumpCapture(Frame, Capture));
    if (Mutation < 40)
      Frame.store(-144 + Mutation, 1, FrameValue::constant(0));
    else if (Mutation == 40)
      Frame.forgetCellValues();
    else
      Frame.merge(FrameState{});
    EXPECT_FALSE(hasEH3JumpCapture(Frame, Capture));
  }
}

TEST(RegistrationJumpState, RuntimeFinallyResumesOnlyFromPrivateArguments) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = jumpFrame(0);
    F.Blocks.resize(8);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0, 0x110f, true}};
    Chain.TryLevelStores = {{0x1120, 0x1127, -1}, {0x1140, 0x1147, 0}};
    auto &Resume = F.Blocks[3];
    Resume.Ops.clear();
    Resume.Succs = {7};
    Resume.EndAddr = 0x1108;
    emitOp(Resume, 0x1105, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
    Resume.InstructionBoundaries = {{0x1105, 3, 0, 1}};
    auto &Active = F.Blocks[7];
    Active.Id = 7;
    Active.StartAddr = 0x1140;
    Active.EndAddr = 0x1147;
    Active.Succs = {5};
    addSlotStore(Active, 0);
    for (unsigned I = 0; I != Active.Ops.size(); ++I)
      Active.Ops[I].Seq = I;
    Active.InstructionBoundaries = {{0x1140, 7, 0, 2}};
    if (Mutation) {
      auto &Jump = F.Blocks[4];
      LowOp Access;
      Access.Addr = 0x110f;
      Access.Opcode = Mutation == 1 ? NdOp::STORE : NdOp::LOAD;
      Access.addInput(NdVar::reg(x86reg::RSP, 4));
      if (Mutation == 1)
        Access.addInput(NdVar::cst(0, 1));
      else
        Access.Output = NdVar::reg(x86reg::RCX, 1);
      Jump.Ops.insert(Jump.Ops.begin(), Access);
      ++Jump.InstructionBoundaries.front().OpCount;
      for (unsigned I = 1; I != Jump.InstructionBoundaries.size(); ++I)
        ++Jump.InstructionBoundaries[I].FirstOp;
      for (unsigned I = 0; I != Jump.Ops.size(); ++I)
        Jump.Ops[I].Seq = I;
    }
    EH3JumpRuntime Runtime;
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<EH3JumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      EH3JumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x111a;
    };
    std::vector<RegistrationSEHControlContract> Controls;
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   &Controls, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    if (!Mutation) {
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(Result.Complete);
      ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
      EXPECT_EQ(Result.SEHJumpResumptions.front().EntryVA, 0x110fu);
      EXPECT_EQ(Result.SEHJumpResumptions.front().TargetVA, 0x1105u);
    } else {
      EXPECT_FALSE(Result.Complete);
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
      EXPECT_TRUE(Result.SEHJumpResumptions.empty());
    }
  }
}

TEST(RegistrationJumpState, ExactFlagsSurviveOnlyTheirReachingDefinition) {
  FrameState State;
  State.OtherRegistersMayBeFrame = true;
  State.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
      FrameValue::constant(1);
  FrameTransfer Transfer(State, -16);
  LowOp Equal;
  Equal.Opcode = NdOp::INT_EQUAL;
  Equal.Output = NdVar::reg(x86reg::ZF, 1);
  Equal.addInput(NdVar::reg(x86reg::RAX, 4));
  Equal.addInput(NdVar::cst(2, 4));
  Transfer.write(Equal, Transfer.evaluate(Equal, true));
  EXPECT_EQ(Transfer.read(Equal.Output).Constant, 0u);
  Equal.Inputs[1] = NdVar::cst(1, 4);
  Transfer.write(Equal, Transfer.evaluate(Equal, true));
  EXPECT_EQ(Transfer.read(Equal.Output).Constant, 1u);
  auto Other = State;
  Other.OtherRegisterBytes[x86reg::ZF] = FrameValue::constant(0);
  State.merge(Other);
  EXPECT_FALSE(Transfer.read(Equal.Output).Constant);
  Transfer.write(Equal, Transfer.evaluate(Equal, true));
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Transfer.write(Call, {});
  EXPECT_FALSE(Transfer.read(Equal.Output).Constant);
  EXPECT_TRUE(Transfer.read(Equal.Output).MayBeFrame);
}

TEST(RegistrationJumpState, AsyncFilterWritesRevokeAParentResumption) {
  for (bool Overlap : {false, true}) {
    SCOPED_TRACE(Overlap);
    auto F = jumpFrame(0);
    F.Blocks.resize(9);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.TryLevelStores = {{0x110f, 0x1116, 0}, {0x1120, 0x1127, -1}};
    auto &Resume = F.Blocks[3];
    Resume.Succs = {8, 5};
    auto &Active = F.Blocks[8];
    Active.Id = 8;
    Active.StartAddr = 0x110f;
    Active.EndAddr = 0x1116;
    Active.Succs = {4};
    addSlotStore(Active, 0);
    for (unsigned I = 0; I != Active.Ops.size(); ++I)
      Active.Ops[I].Seq = I;
    Active.InstructionBoundaries = {{0x110f, 7, 0, 2}};
    auto &Jump = F.Blocks[4];
    Jump.StartAddr += 0x41;
    Jump.EndAddr += 0x41;
    for (auto &Op : Jump.Ops)
      Op.Addr += 0x41;
    for (auto &Boundary : Jump.InstructionBoundaries)
      Boundary.Address += 0x41;
    auto &Filter = F.Blocks[7];
    Filter.Id = 7;
    Filter.StartAddr = 0x1800;
    Filter.EndAddr = 0x1806;
    emitOp(Filter, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4),
            NdVar::cst(uint32_t(Overlap ? -144 : -28), 4)});
    emitOp(Filter, 0x1800, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(1, 4)});
    emitOp(Filter, 0x1805, NdOp::RETURN, {}, {});
    Filter.InstructionBoundaries = {{0x1800, 5, 0, 2}, {0x1805, 1, 2, 1}};
    Filter.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    Filter.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    EH3JumpRuntime Runtime;
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<EH3JumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      EH3JumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x115b;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    EXPECT_EQ(Result.Complete, !Overlap);
    if (!Overlap && !Result.Complete)
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
    EXPECT_EQ(Result.SEHJumpResumptionsComplete, !Overlap);
    EXPECT_EQ(Result.SEHJumpResumptions.size(), Overlap ? 0u : 1u);
  }
}

TEST(RegistrationCRTCall, ControlFPWritesOnlyADisjointScalarWord) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = jumpFrame(0);
    auto &Setup = F.Blocks[1];
    emitOp(
        Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
        {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Mutation == 1   ? -24
                                                         : Mutation == 2 ? -192
                                                                         : -48),
                                                4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(0, 4)});
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::tmp(0, 4),
            Mutation == 3 ? NdVar::reg(x86reg::RBP, 4) : NdVar::cst(0, 4)});
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
    emitOp(
        Setup, 0x1086, NdOp::STORE, {},
        {NdVar::tmp(0, 4), NdVar::cst(Mutation == 4 ? 0x30000 : 0x8001f, 4)});
    Setup.InstructionBoundaries.back().OpCount = 7;
    auto &Call = F.Blocks[2];
    Call.Ops[0].Opcode = NdOp::INDIR_CALL;
    Call.Ops[0].Inputs[0] = NdVar::cst(0x4200, 4);
    Call.InstructionBoundaries[0].Immediate = 0x4200;
    Call.InstructionBoundaries[0].ControlFlags |=
        LowInstructionControlFlag::Indirect;
    auto &Resume = F.Blocks[3];
    Resume.Ops.erase(Resume.Ops.begin());
    Resume.Ops.pop_back();
    Resume.Succs = {5};
    Resume.EndAddr = 0x110b;
    Resume.InstructionBoundaries = {{0x1108, 3, 0, 1}};
    EH3JumpRuntime Runtime;
    Runtime.ControlFP = [Mutation](const LowOp &Op, size_t &) {
      return Mutation != 5 && Op.Opcode == NdOp::INDIR_CALL &&
             Op.Addr == 0x1100;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    EXPECT_EQ(Result.HasBoundSEHCallees, Mutation == 0);
    if (!Mutation)
      EXPECT_TRUE(Result.Complete);
  }
}

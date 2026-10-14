//===- SymSegmentedMemoryTests.cpp - Explicit linear memory bases ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Symbolic x64 segment bases retain process-memory aliases and dependencies.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <limits>

using namespace neverd;
using namespace neverd::symbolic;

namespace {
constexpr auto FS = NdMemoryAddressSpace::X86FS;
constexpr auto GS = NdMemoryAddressSpace::X86GS;
constexpr auto Default = NdMemoryAddressSpace::Default;

LowOp memory(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
             NdMemoryAddressSpace Space = GS) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.MemoryAddressSpace = Space;
  for (auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}

TEST(SymSegmentedMemory, BasesAreExplicitValidatedAndPartOfForkIdentity) {
  SymContext Ctx;
  SymState State(Ctx);
  const auto Base = Ctx.mkVar("segment_base", 64);
  EXPECT_FALSE(State.x64MemoryAddressSpaceBase(FS));
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(FS, Base));
  for (const auto Invalid : {SymRef(), SymRef(UINT32_MAX - 1),
                             Ctx.mkConst(32, 0), Ctx.mkConst(128, 0)}) {
    EXPECT_FALSE(State.setX64MemoryAddressSpaceBase(FS, Invalid));
    EXPECT_EQ(State.x64MemoryAddressSpaceBase(FS), Base);
  }
  for (auto Space : {Default, static_cast<NdMemoryAddressSpace>(255)}) {
    EXPECT_FALSE(State.setX64MemoryAddressSpaceBase(Space, Base));
    EXPECT_FALSE(State.x64MemoryAddressSpaceBase(Space));
  }
  auto Copy = State;
  EXPECT_TRUE(State.mergeIdentical(Copy));
  ASSERT_TRUE(Copy.setX64MemoryAddressSpaceBase(GS, Ctx.mkConst(64, 0)));
  EXPECT_FALSE(State.mergeIdentical(Copy));
  Copy = State;
  ASSERT_TRUE(Copy.setX64MemoryAddressSpaceBase(FS, Ctx.mkVar("other", 64)));
  EXPECT_FALSE(State.mergeIdentical(Copy));
  Copy.clearX64MemoryAddressSpaceBases();
  EXPECT_FALSE(Copy.x64MemoryAddressSpaceBase(FS));
  EXPECT_FALSE(Copy.x64MemoryAddressSpaceBase(GS));
  EXPECT_FALSE(State.mergeIdentical(Copy));
  EXPECT_EQ(State.x64MemoryAddressSpaceBase(FS), Base);
}

TEST(SymSegmentedMemory, ConcreteAddressesPreserveBaseCarryAndOffsetWidth) {
  struct Case {
    uint64_t Base, Offset, Linear;
    uint16_t OffsetBytes;
  };
  const Case Cases[] = {{0, 0x4020, 0x4020, 8},
                        {0x4000, 0x20, 0x4020, 8},
                        {0x100000001, 0xffffffff, 0x200000000, 4},
                        {0x5000, UINT64_MAX - 0xfff, 0x4000, 8},
                        {UINT64_MAX - 0xfff, 0x2000, 0x1000, 8}};
  for (auto Space : {FS, GS}) {
    for (const auto &C : Cases) {
      SCOPED_TRACE(C.Base);
      SymContext Ctx;
      SymState State(Ctx);
      SymExec Exec(Ctx, State);
      ASSERT_TRUE(
          State.setX64MemoryAddressSpaceBase(Space, Ctx.mkConst(64, C.Base)));
      const auto Address = Ctx.mkConst(64, C.Linear);
      const auto Value = Ctx.mkConst(64, 0x1122334455667788);
      ASSERT_TRUE(State.store(Address, Value));
      const auto Load = memory(NdOp::LOAD, NdVar::reg(0, 8),
                               {NdVar::cst(C.Offset, C.OffsetBytes)}, Space);
      EXPECT_EQ(Exec.memoryAddressValue(Load), Address);
      ASSERT_EQ(Exec.step(Load), StepResult::Continue);
      EXPECT_EQ(State.read(SymSpace::Register, 0, 8), Value);
      EXPECT_EQ(Exec.unmodelledCount(), 0u);
    }
  }
}

TEST(SymSegmentedMemory, SymbolicBaseIsRequiredByTheSolverAddressRelation) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Base = Ctx.mkVar("base", 64);
  const auto Offset = State.read(SymSpace::Register, 8, 4);
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(GS, Base));
  const auto Load = memory(NdOp::LOAD, NdVar::reg(0, 8), {NdVar::reg(8, 4)});
  const auto Linear = Exec.memoryAddressValue(Load);
  const auto Extended = Ctx.mkZExtOrTrunc(Offset, 64);
  // Independently expressed carry identity for arbitrary base and offset.
  const auto Expected =
      Ctx.mkAdd(Ctx.mkXor(Base, Extended),
                Ctx.mkShl(Ctx.mkAnd(Base, Extended), Ctx.mkConst(64, 1)));
  EXPECT_EQ(solver::checkEqual(Ctx, Linear, Expected),
            solver::EquivResult::Equal);
  solver::BitVectorModel Counterexample;
  ASSERT_EQ(solver::checkEqual(Ctx, Linear, Extended, &Counterexample),
            solver::EquivResult::Different);
  const auto Values = Counterexample.asVarValues(Ctx);
  EXPECT_NE(Ctx.eval(Linear, Values), Ctx.eval(Extended, Values));
}

TEST(SymSegmentedMemory, RepeatedReadsKeepLinearMemoryProvenance) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Base = Ctx.mkVar("base", 64);
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(GS, Base));
  const auto Load = memory(NdOp::LOAD, NdVar::reg(0, 8), {NdVar::cst(24, 8)});
  ASSERT_EQ(Exec.step(Load), StepResult::Continue);
  const auto First = State.read(SymSpace::Register, 0, 8);
  ASSERT_TRUE(Ctx.isVar(First));
  const auto *Origin = State.loadOrigin(First);
  ASSERT_NE(Origin, nullptr);
  EXPECT_EQ(Origin->Address, Ctx.mkAdd(Base, Ctx.mkConst(64, 24)));
  EXPECT_EQ(Origin->Bytes, 8u);
  ASSERT_EQ(Exec.step(Load), StepResult::Continue);
  EXPECT_EQ(State.read(SymSpace::Register, 0, 8), First);
  const auto Inputs = State.memoryInputOrigins(First);
  ASSERT_EQ(Inputs.size(), 1u);
  EXPECT_EQ(Inputs[0].Base, Base);
  EXPECT_EQ(Inputs[0].Offset, 24u);
}

TEST(SymSegmentedMemory, DefaultFSAndGSSharePartialStoreAliases) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Base = Ctx.mkVar("base", 64);
  const auto Input = Ctx.mkVar("input", 64);
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(FS, Base));
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(GS, Base));
  State.write(SymSpace::Register, 0, Input);
  State.write(SymSpace::Register, 8, Ctx.mkAdd(Base, Ctx.mkConst(64, 16)));
  ASSERT_EQ(Exec.step(memory(NdOp::STORE, {},
                             {NdVar::cst(16, 8), NdVar::reg(0, 8)}, FS)),
            StepResult::Continue);
  // The legacy space token is metadata; input 1 is the segment offset.
  ASSERT_EQ(Exec.step(memory(
                NdOp::STORE, {},
                {NdVar::cst(0, 8), NdVar::cst(19, 8), NdVar::cst(0xbeef, 2)})),
            StepResult::Continue);
  ASSERT_EQ(Exec.step(memory(NdOp::LOAD, NdVar::reg(16, 8), {NdVar::reg(8, 8)},
                             Default)),
            StepResult::Continue);
  const auto Expected =
      Ctx.mkOr(Ctx.mkAnd(Input, Ctx.mkConst(64, ~UINT64_C(0xffff000000))),
               Ctx.mkConst(64, UINT64_C(0xbeef000000)));
  EXPECT_EQ(
      solver::checkEqual(Ctx, State.read(SymSpace::Register, 16, 8), Expected),
      solver::EquivResult::Equal);
  EXPECT_EQ(Exec.opaqueOperationCount(), 0u);
}

TEST(SymSegmentedMemory, DistinctSymbolicBasesDoNotImplyDisjointMemory) {
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  const auto Left = Ctx.mkVar("left", 64);
  const auto Right = Ctx.mkVar("right", 64);
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(FS, Left));
  ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(GS, Right));
  State.store(Right, Ctx.mkConst(32, 7));
  ASSERT_EQ(Exec.step(memory(NdOp::STORE, {},
                             {NdVar::cst(0, 8), NdVar::cst(9, 4)}, FS)),
            StepResult::Continue);
  ASSERT_EQ(Exec.step(memory(NdOp::LOAD, NdVar::reg(0, 4), {NdVar::cst(0, 8)})),
            StepResult::Continue);
  EXPECT_FALSE(Ctx.isConst(State.read(SymSpace::Register, 0, 4)));
  EXPECT_EQ(Exec.memoryHavocCount(), 1u);
}

TEST(SymSegmentedMemory, AtomicsUseTheSameLinearAddressAndFullOldValue) {
  for (auto Opcode :
       {NdOp::ATOMIC_XCHG, NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG}) {
    for (bool Match : {false, true}) {
      SymContext Ctx;
      SymState State(Ctx);
      SymExec Exec(Ctx, State);
      ASSERT_TRUE(
          State.setX64MemoryAddressSpaceBase(GS, Ctx.mkConst(64, 0x4000)));
      const auto Address = Ctx.mkConst(64, 0x4020);
      ASSERT_TRUE(State.store(Address, Ctx.mkConst(32, 5)));
      auto Op = memory(Opcode, NdVar::reg(0, 4),
                       {NdVar::cst(0x20, 8), NdVar::cst(3, 4)});
      if (Opcode == NdOp::ATOMIC_CMPXCHG) {
        Op.Inputs[1] = NdVar::cst(Match ? 5 : 7, 4);
        Op.addInput(NdVar::cst(3, 4));
      }
      Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      ASSERT_EQ(Exec.step(Op), StepResult::Continue);
      EXPECT_EQ(State.read(SymSpace::Register, 0, 4), Ctx.mkConst(32, 5));
      const auto Expected = Opcode == NdOp::ATOMIC_ADD                 ? 8
                            : Opcode == NdOp::ATOMIC_CMPXCHG && !Match ? 5
                                                                       : 3;
      EXPECT_EQ(State.load(Address, 4), Ctx.mkConst(32, Expected));
      EXPECT_EQ(Exec.unmodelledCount(), 0u);
    }
  }
}

TEST(SymSegmentedMemory, CallsAndOpaqueSystemEffectsRevokeBases) {
  for (unsigned Case = 0; Case != 6; ++Case) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(FS, Ctx.mkConst(64, 0x4000)));
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(GS, Ctx.mkConst(64, 0x5000)));
    const auto Opcode = Case < 2   ? NdOp::CALL
                        : Case < 4 ? NdOp::INDIR_CALL
                                   : NdOp::INTRINSIC;
    auto Op =
        memory(Opcode, {},
               {NdVar::cst(Case < 4 ? 0x1234
                                    : uint64_t(Case == 4 ? Intrinsic::Swapgs
                                                         : Intrinsic::Wrmsr),
                           Case < 4 ? 8 : 2)},
               Default);
    if (Opcode == NdOp::INDIR_CALL)
      Op.Inputs[0] = NdVar::reg(8, 8);
    SymCallEffect Summary;
    Summary.Memory = SymCallMemoryEffect::Preserve;
    EXPECT_EQ(Exec.step(Op, Case == 1 || Case == 3 ? &Summary : nullptr),
              Case < 4 ? StepResult::Continue : StepResult::Unmodelled);
    EXPECT_FALSE(State.x64MemoryAddressSpaceBase(FS));
    EXPECT_FALSE(State.x64MemoryAddressSpaceBase(GS));
    EXPECT_EQ(
        Exec.step(memory(NdOp::LOAD, NdVar::reg(0, 4), {NdVar::cst(16, 8)})),
        StepResult::Unmodelled);
    EXPECT_EQ(Exec.step(memory(NdOp::STORE, {},
                               {NdVar::cst(16, 8), NdVar::cst(0, 4)})),
              StepResult::Unmodelled);
    EXPECT_TRUE(State.memoryIsUnknown());
    ASSERT_TRUE(
        State.setX64MemoryAddressSpaceBase(FS, Ctx.mkConst(64, 0x6000)));
    ASSERT_TRUE(State.store(Ctx.mkConst(64, 0x6010), Ctx.mkConst(32, 7)));
    ASSERT_EQ(Exec.step(memory(NdOp::LOAD, NdVar::reg(0, 4),
                               {NdVar::cst(16, 8)}, FS)),
              StepResult::Continue);
    EXPECT_EQ(State.read(SymSpace::Register, 0, 4), Ctx.mkConst(32, 7));
    EXPECT_FALSE(State.x64MemoryAddressSpaceBase(GS));
  }
}

TEST(SymSegmentedMemory, MalformedOperandsNeverBecomeBasedMemoryAccesses) {
  const auto Good = memory(NdOp::LOAD, NdVar::reg(0, 4), {NdVar::cst(0, 8)});
  std::array<LowOp, 8> Bad;
  Bad.fill(Good);
  Bad[0].Inputs[0].Size = 0;
  Bad[1].Inputs[0] = NdVar::reg(8, 16);
  Bad[2].Output = NdVar::cst(0, 4);
  Bad[3].addInput(NdVar::cst(1, 8));
  Bad[3].addInput(NdVar::cst(2, 8));
  Bad[4].Inputs[0] = NdVar::reg(8, 8);
  Bad[4].addInput(NdVar::cst(0, 8));
  Bad[5].MemoryAddressSpace = static_cast<NdMemoryAddressSpace>(255);
  Bad[6] = memory(NdOp::STORE, NdVar::reg(0, 4),
                  {NdVar::cst(0, 8), NdVar::cst(1, 4)});
  Bad[7] = memory(
      NdOp::ATOMIC_CMPXCHG, NdVar::reg(0, 4),
      {NdVar::cst(0, 8), NdVar::cst(1, 4), NdVar::cst(2, 4), NdVar::cst(3, 4)});
  for (const auto &Op : Bad) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(GS, Ctx.mkConst(64, 0)));
    EXPECT_FALSE(Exec.memoryAddressValue(Op));
    EXPECT_EQ(Exec.step(Op), StepResult::Unmodelled);
    EXPECT_EQ(State.memoryIsUnknown(), Op.Opcode != NdOp::LOAD);
  }
}

TEST(SymSegmentedMemory, WrappingAccessExtentRefusesWithoutInvalidResults) {
  for (auto Space : {Default, GS}) {
    for (auto Opcode : {NdOp::LOAD, NdOp::STORE, NdOp::ATOMIC_ADD}) {
      SymContext Ctx;
      SymState State(Ctx);
      SymExec Exec(Ctx, State);
      ASSERT_TRUE(State.setX64MemoryAddressSpaceBase(GS, Ctx.mkConst(64, 0)));
      const auto Saved = Ctx.mkConst(32, 0x12345678);
      State.write(SymSpace::Register, 0, Saved);
      ASSERT_TRUE(State.store(Ctx.mkConst(64, 0x4000), Saved));
      auto Op =
          memory(Opcode, Opcode == NdOp::STORE ? NdVar{} : NdVar::reg(0, 4),
                 {NdVar::cst(UINT64_MAX - 1, 8)}, Space);
      if (Opcode != NdOp::LOAD)
        Op.addInput(NdVar::cst(1, 4));
      EXPECT_EQ(Exec.step(Op), StepResult::Unmodelled);
      const auto After = State.read(SymSpace::Register, 0, 4);
      ASSERT_TRUE(After);
      EXPECT_EQ(Ctx.width(After), 32u);
      EXPECT_EQ(Ctx.isVar(After), Opcode != NdOp::STORE);
      EXPECT_EQ(State.memoryIsUnknown(), Opcode != NdOp::LOAD);
    }
  }
}
} // namespace

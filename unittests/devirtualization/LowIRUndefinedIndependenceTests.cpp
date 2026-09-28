//===- LowIRUndefinedIndependenceTests.cpp - Relational certificates
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRUndefinedIndependence.h"

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRIndependenceStatus;

NdVar reg(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar number(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp op(NdOp Code, NdVar Output = {},
         std::initializer_list<NdVar> Inputs = {}) {
  LowOp Value;
  Value.Opcode = Code;
  Value.Output = Output;
  for (const auto &Input : Inputs)
    Value.addInput(Input);
  return Value;
}

struct Program {
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  Program() {
    Function.Entry = 0x100;
    Contract.ReturnRegisters = {{0, 8}};
    block(0, 0x100);
  }

  void block(int Id, va_t Address, std::vector<int> Successors = {}) {
    LowBlock B;
    B.Id = Id;
    B.StartAddr = B.EndAddr = Address;
    B.Succs = std::move(Successors);
    Function.Blocks.push_back(std::move(B));
  }

  LowIRUndefinedInstruction &instruction(std::initializer_list<LowOp> Ops) {
    auto &B = Function.Blocks.back();
    LowInstructionBoundary Boundary;
    Boundary.Address = B.EndAddr++;
    Boundary.Size = 1;
    Boundary.FirstOp = B.Ops.size();
    Boundary.OpCount = Ops.size();
    for (auto Value : Ops) {
      Value.Addr = Boundary.Address;
      Value.Seq = static_cast<int>(B.Ops.size() - Boundary.FirstOp);
      if (Value.Opcode == NdOp::BRANCH || Value.Opcode == NdOp::COND_BR ||
          Value.Opcode == NdOp::INDIR_BR) {
        Boundary.Control = LowInstructionControl::Branch;
        Boundary.ControlFlags = LowInstructionControlFlag::Branch;
        if (Value.Opcode == NdOp::COND_BR)
          Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
        if (Value.Opcode == NdOp::INDIR_BR)
          Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
        else
          Boundary.Immediate = Value.Inputs[0].Offset;
      } else if (Value.Opcode == NdOp::RETURN) {
        Boundary.Control = LowInstructionControl::Return;
        Boundary.ControlFlags = LowInstructionControlFlag::Return;
      }
      B.Ops.push_back(Value);
    }
    B.InstructionBoundaries.push_back(Boundary);
    LowIRUndefinedInstruction Record;
    Record.BlockId = B.Id;
    Record.Boundary = Boundary;
    Record.Effects.Coverage = LowUndefinedCoverage::Complete;
    Record.Effects.OpCount = Ops.size();
    Record.Effects.OperationDigest = lowUndefinedOperationDigest(
        llvm::ArrayRef<LowOp>(B.Ops).slice(Boundary.FirstOp, Boundary.OpCount));
    Records.push_back(std::move(Record));
    return Records.back();
  }

  void arbitrary(NdVar Output, uint16_t BitOffset = 0, uint16_t BitCount = 1) {
    auto &Record = instruction({op(NdOp::NOP)});
    Record.Effects.Effects.push_back({1, Output, BitOffset, BitCount, {}});
  }

  void finish() { instruction({op(NdOp::RETURN)}); }

  void rebind() {
    for (auto &R : Records)
      for (const auto &B : Function.Blocks)
        if (B.Id == R.BlockId)
          R.Effects.OperationDigest =
              lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(B.Ops).slice(
                  R.Boundary.FirstOp, R.Boundary.OpCount));
  }

  void frame() { Contract.Frame = LowIRIndependenceFrame{{32, 8}, -16, 16}; }

  LowIRIndependenceResult check(const LowIRIndependenceLimits &Limits = {}) {
    return checkLowIRUndefinedIndependence(Function, Records, Contract, Limits);
  }
};

void expectStatus(Program &P, Status Expected,
                  const LowIRIndependenceLimits &Limits = {}) {
  const auto Result = P.check(Limits);
  EXPECT_EQ(Result.Status, Expected) << Result.Diagnostic;
  EXPECT_EQ(Result.Certificate.has_value(), Expected == Status::Proved);
}

TEST(LowIRUndefinedIndependence,
     CopiesShareOneProducerButSeparateEffectsDoNot) {
  Program Shared;
  Shared.arbitrary(reg(8), 0, 64);
  Shared.instruction({op(NdOp::COPY, reg(16), {reg(8)}),
                      op(NdOp::INT_XOR, reg(0), {reg(8), reg(16)})});
  Shared.finish();
  expectStatus(Shared, Status::Proved);

  Program Fresh;
  Fresh.arbitrary(reg(8), 0, 64);
  Fresh.instruction({op(NdOp::COPY, reg(16), {reg(8)})});
  // Reusing the destination must create a new producer, not reuse its name.
  Fresh.arbitrary(reg(8), 0, 64);
  Fresh.instruction({op(NdOp::INT_XOR, reg(0), {reg(8), reg(16)})});
  Fresh.finish();
  expectStatus(Fresh, Status::Dependent);
}

TEST(LowIRUndefinedIndependence, PartialRegisterOverwritePreservesOtherBits) {
  Program P;
  P.arbitrary(reg(0), 32, 1);
  P.instruction({op(NdOp::COPY, reg(0, 4), {number(0, 4)})});
  P.finish();
  expectStatus(P, Status::Dependent);
  P.Contract.ReturnRegisters = {{0, 4}};
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence,
     SpillReloadKeepsCorrelationAndWrittenBytesCount) {
  Program P;
  P.frame();
  P.arbitrary(reg(8), 0, 64);
  const auto Address = NdVar::tmp(0, 8);
  P.instruction({op(NdOp::INT_SUB, Address, {reg(32), number(4)}),
                 op(NdOp::STORE, {}, {Address, reg(8)}),
                 op(NdOp::LOAD, reg(16), {Address}),
                 op(NdOp::INT_XOR, reg(0), {reg(16), reg(8)})});
  P.finish();
  expectStatus(P,
               Status::Dependent); // The final spill is observable by default.
  P.Contract.ObserveWrittenFrameBytes = false;
  expectStatus(P, Status::Proved);

  Program Killed;
  Killed.frame();
  Killed.arbitrary(reg(8), 0, 64);
  Killed.instruction({op(NdOp::INT_SUB, Address, {reg(32), number(4)}),
                      op(NdOp::STORE, {}, {Address, reg(8)}),
                      op(NdOp::LOAD, reg(16), {Address}),
                      op(NdOp::INT_XOR, reg(0), {reg(16), reg(8)}),
                      op(NdOp::STORE, {}, {Address, number(0)})});
  Killed.finish();
  expectStatus(Killed, Status::Proved);
}

TEST(LowIRUndefinedIndependence, PartialFrameOverwriteDoesNotEraseHighBytes) {
  Program P;
  P.frame();
  P.arbitrary(reg(8), 0, 64);
  P.instruction({op(NdOp::STORE, {}, {reg(32), reg(8)}),
                 op(NdOp::STORE, {}, {reg(32), number(0, 4)}),
                 op(NdOp::LOAD, reg(0, 4), {reg(32)})});
  P.finish();
  P.Contract.ReturnRegisters = {{0, 4}};
  expectStatus(P, Status::Dependent);
  P.Contract.ObserveWrittenFrameBytes = false;
  expectStatus(P, Status::Proved);
  P.Contract.ReturnRegisters = {{0, 8}};
  // Changing the LOAD, not only the observation, exposes the surviving bytes.
  P.Function.Blocks[0].Ops[3].Output = reg(0);
  P.rebind();
  expectStatus(P, Status::Dependent);
}

Program diamond(NdVar Condition) {
  Program P;
  P.Function.Blocks[0].Succs = {1, 2};
  P.instruction({op(NdOp::COND_BR, {}, {number(0x200), Condition})});
  P.block(1, 0x200);
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  P.block(2, 0x300);
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  return P;
}

TEST(LowIRUndefinedIndependence, BranchIsCheckedBeforeItsOwnPathAssumption) {
  auto P = diamond(reg(8, 1));
  P.Records[0].Effects.Effects.push_back({0, reg(8, 1), 0, 8, {}});
  // Both arms return zero; the strict certificate still observes control.
  expectStatus(P, Status::Dependent);
  auto Shared = diamond(reg(8, 1));
  const auto Result = Shared.check();
  EXPECT_EQ(Result.Status, Status::Proved) << Result.Diagnostic;
  EXPECT_EQ(Result.Paths, 2u);
}

TEST(LowIRUndefinedIndependence, UnreachableUnsupportedArmDoesNotPoisonProof) {
  auto P = diamond(number(0, 1));
  P.Records[1].Effects.Coverage = LowUndefinedCoverage::Unsupported;
  P.Records[1].Effects.Diagnostic = "synthetic unsupported instruction";
  expectStatus(P, Status::Proved);
  P.Function.Blocks[0].Ops[0].Inputs[1] = number(1, 1);
  P.rebind();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, ReachableCyclesRefuseWithoutUnrolling) {
  Program P;
  P.Function.Blocks[0].Succs = {0, 1};
  P.instruction({op(NdOp::COND_BR, {}, {number(0x100), number(0, 1)})});
  P.block(1, 0x200);
  P.finish();
  expectStatus(P, Status::Proved);
  P.Function.Blocks[0].Ops[0].Inputs[1] = reg(8, 1);
  P.rebind();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence, SidecarMustBindCompleteBoundaryAndCoverage) {
  Program P;
  P.finish();
  P.Records[0].Effects.Coverage = LowUndefinedCoverage::Missing;
  expectStatus(P, Status::Unsupported);
  P.Records[0].Effects.Coverage = LowUndefinedCoverage::Complete;
  ++P.Records[0].Effects.OpCount;
  expectStatus(P, Status::Invalid);
  --P.Records[0].Effects.OpCount;
  ++P.Records[0].Boundary.Size;
  expectStatus(P, Status::Invalid);
  --P.Records[0].Boundary.Size;
  P.Records.clear();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence,
     ConditionalProducerUsesDefinedBooleanSnapshot) {
  Program P;
  auto &R =
      P.instruction({op(NdOp::INT_EQUAL, NdVar::tmp(0, 1), {reg(8), number(1)}),
                     op(NdOp::COPY, reg(0), {number(0)})});
  R.Effects.Effects.push_back({1, reg(0), 0, 1, NdVar::tmp(0, 1)});
  P.finish();
  expectStatus(P, Status::Proved); // The later ordinary write kills the choice.
  P.Records[0].Effects.Effects[0].When = reg(8, 1);
  expectStatus(P, Status::Invalid);
  P.Records[0].Effects.Effects[0].When = NdVar::tmp(2, 1);
  expectStatus(P, Status::Invalid);
  P.Records[0].Effects.Effects[0].When = number(2, 1);
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, FalseConditionalProducerPreservesOldValue) {
  Program P;
  auto &R = P.instruction({op(NdOp::COPY, reg(0), {number(42)})});
  R.Effects.Effects.push_back({1, reg(0), 0, 64, number(0, 1)});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Records[0].Effects.Effects[0].When = number(1, 1);
  expectStatus(P, Status::Dependent);
}

TEST(LowIRUndefinedIndependence,
     TemporaryIdentityCannotLeakAcrossInstructions) {
  Program P;
  P.instruction({op(NdOp::COPY, NdVar::tmp(0, 8), {number(0)})});
  P.instruction({op(NdOp::COPY, reg(0), {NdVar::tmp(0, 8)})});
  P.finish();
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, UnknownAliasAndDependentAddressRefuse) {
  Program P;
  P.frame();
  P.instruction({op(NdOp::STORE, {}, {reg(48), number(0)})});
  P.finish();
  expectStatus(P, Status::Unsupported);

  Program Choice;
  Choice.frame();
  Choice.arbitrary(reg(8), 0, 64);
  Choice.instruction({op(NdOp::LOAD, reg(0), {reg(8)})});
  Choice.finish();
  expectStatus(Choice, Status::Dependent);
}

TEST(LowIRUndefinedIndependence, EntryNonwrappingContractIsNotVacuousSuccess) {
  Program P;
  P.frame();
  P.Contract.EntryConstants.push_back({reg(32), 0});
  P.finish();
  expectStatus(P, Status::InfeasibleEntry);
  P.Contract.EntryConstants[0].Value = 0x1000;
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence, AllBudgetsFailWithoutPublishingCertificate) {
  Program P;
  P.frame();
  P.arbitrary(reg(8));
  P.finish();
  LowIRIndependenceLimits L;
  L.MaxOperations = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxProducers = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxFrameBytes = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxSolverQueries = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxPaths = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxBlockVisits = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxSymbolicNodes = 0;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxInstructions = 1;
  expectStatus(P, Status::BudgetExceeded, L);
  L = {};
  L.MaxObservations = 0;
  expectStatus(P, Status::BudgetExceeded, L);
}

TEST(LowIRUndefinedIndependence, GraphMetadataIsBoundedAndWellFormed) {
  Program P;
  P.finish();
  LowIRIndependenceLimits Limits;
  Limits.MaxBlockVisits = 1;
  P.Function.ModuleAnalysisRoots = {P.Function.Entry, P.Function.Entry + 1};
  expectStatus(P, Status::BudgetExceeded, Limits);
  P.Function.ModuleAnalysisRoots.clear();
  P.Function.Blocks.front().Succs = {1, 2, 3};
  expectStatus(P, Status::BudgetExceeded, Limits);
  P.Function.Blocks.front().Succs = {1};
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, PartialOverwriteUsesDeclaredByteOrder) {
  Program P;
  P.arbitrary(reg(0), 0, 1);
  P.instruction({op(NdOp::COPY, reg(0, 1), {number(0, 1)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Contract.ByteOrder = llvm::endianness::big;
  expectStatus(P, Status::Dependent);
  P.Function.Blocks[0].Ops[1].Output = reg(7, 1);
  P.rebind();
  expectStatus(P, Status::Proved);
}

TEST(LowIRUndefinedIndependence, CertificateBindsOperationsEffectsAndContract) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  auto A = P.check();
  ASSERT_TRUE(A.proved()) << A.Diagnostic;
  ASSERT_TRUE(A.Certificate);
  EXPECT_EQ(A.Certificate->Scope, LowIRIndependenceScope::CompleteAcyclicLowIR);
  EXPECT_EQ(A.Certificate->Instructions.size(), P.Records.size());
  EXPECT_EQ(A.Certificate->InputDigest.size(), 64u);
  P.Function.Blocks[0].Ops[0].Inputs[0] = number(1);
  expectStatus(P, Status::Invalid); // Same-length code cannot reuse a sidecar.
  P.rebind();
  auto B = P.check();
  ASSERT_TRUE(B.proved()) << B.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
  P.Records[0].Effects.Effects.push_back({0, reg(8), 0, 1, {}});
  auto C = P.check();
  ASSERT_TRUE(C.proved()) << C.Diagnostic;
  EXPECT_NE(B.Certificate->InputDigest, C.Certificate->InputDigest);
  P.Contract.ObserveWrittenFrameBytes = false;
  auto D = P.check();
  ASSERT_TRUE(D.proved()) << D.Diagnostic;
  EXPECT_NE(C.Certificate->InputDigest, D.Certificate->InputDigest);
  P.Function.ModuleAnalysisRoots.insert(P.Function.Entry);
  auto E = P.check();
  ASSERT_TRUE(E.proved()) << E.Diagnostic;
  EXPECT_NE(D.Certificate->InputDigest, E.Certificate->InputDigest);
  P.Function.OrdinaryModuleAnalysisRoots.insert(P.Function.Entry);
  auto F = P.check();
  ASSERT_TRUE(F.proved()) << F.Diagnostic;
  EXPECT_NE(E.Certificate->InputDigest, F.Certificate->InputDigest);
  LowIRIndependenceLimits Limits;
  Limits.MaxOperations += 1;
  auto G = P.check(Limits);
  ASSERT_TRUE(G.proved()) << G.Diagnostic;
  EXPECT_EQ(G.Certificate->Limits.MaxOperations, Limits.MaxOperations);
  EXPECT_NE(F.Certificate->InputDigest, G.Certificate->InputDigest);
}

TEST(LowIRUndefinedIndependence, OpaqueTerminatorMetadataCannotBecomeNop) {
  Program P;
  P.instruction({op(NdOp::NOP)});
  auto &Boundary = P.Function.Blocks[0].InstructionBoundaries[0];
  Boundary.Control = LowInstructionControl::Terminator;
  Boundary.ControlFlags = LowInstructionControlFlag::Terminator;
  P.Records[0].Boundary = Boundary;
  P.finish();
  expectStatus(P, Status::Unsupported);
}

TEST(LowIRUndefinedIndependence,
     MalformedOperandCapacityAndWrappingTemporaryRefuse) {
  Program P;
  P.instruction({op(NdOp::COPY, reg(0), {number(0)})});
  P.finish();
  P.Function.Blocks[0].Ops[0].NumInputs = 7;
  expectStatus(P, Status::Invalid);
  P.Function.Blocks[0].Ops[0].NumInputs = 1;
  P.Function.Blocks[0].Ops[0].Inputs[0] = NdVar::tmp(UINT64_MAX, 1);
  P.rebind();
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence,
     EmptyOperationSpansStillConsumeInstructionBudget) {
  Program P;
  P.instruction({});
  P.instruction({});
  P.finish();
  expectStatus(P, Status::Proved);
  LowIRIndependenceLimits L;
  L.MaxInstructions = 2;
  expectStatus(P, Status::BudgetExceeded, L);
}

TEST(LowIRUndefinedIndependence, CorrelationsSurviveControlEdges) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.arbitrary(reg(8), 0, 64);
  P.instruction({op(NdOp::COPY, reg(16), {reg(8)}),
                 op(NdOp::BRANCH, {}, {number(0x200)})});
  P.block(1, 0x200);
  P.instruction({op(NdOp::INT_XOR, reg(0), {reg(8), reg(16)})});
  P.finish();
  expectStatus(P, Status::Proved);
  P.Function.Blocks[1].Ops[0].Inputs[1] = number(0);
  P.rebind();
  expectStatus(P, Status::Dependent);
}

TEST(LowIRUndefinedIndependence,
     IndependentDynamicTargetNeedsItsOwnResolutionProof) {
  Program P;
  P.Function.Blocks[0].Succs = {1};
  P.instruction({op(NdOp::INDIR_BR, {}, {reg(8)})});
  P.block(1, 0x200);
  P.finish();
  // A finite CFG successor list must not justify an unproved target value.
  expectStatus(P, Status::Unsupported);
  P.Contract.EntryConstants.push_back({reg(8), 0x200});
  expectStatus(P, Status::Proved);
  P.Contract.EntryConstants[0].Value = 0x201;
  expectStatus(P, Status::Invalid);
}

TEST(LowIRUndefinedIndependence, AdditionalEntriesAreNotSilentlyOmitted) {
  auto P = diamond(number(0, 1));
  P.Function.ModuleAnalysisRoots = {0x100};
  P.Function.OrdinaryModuleAnalysisRoots = {0x100};
  expectStatus(P, Status::Proved);
  P.Function.OrdinaryModuleAnalysisRoots.insert(0x200);
  expectStatus(P, Status::Unsupported);
}
} // namespace

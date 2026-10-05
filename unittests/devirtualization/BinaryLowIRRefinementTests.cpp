//===- BinaryLowIRRefinementTests.cpp - Original-to-residual relations
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <set>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRRefinementStatus;
using Witness = LowIRRefinementWitness;
constexpr va_t Entry = 0x1000;

struct Program {
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceContract Contract;

  Program(std::initializer_list<uint8_t> Bytes) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    Segment Code;
    Code.VA = Entry;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data = Bytes;
    Code.Size = Code.FileSz = Code.Data.size();
    Image.Segments.push_back(std::move(Code));
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    Options.X64CetDisabled = true;
    Options.X64FlagsProfile = Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::UserX64NoFaultV1;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -64, 8};
    for (unsigned I = 0; I != 16; ++I)
      Contract.ReturnRegisters.push_back({I * 8, 8});
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF, x86reg::DF})
      Contract.ReturnRegisters.push_back({Flag, 1});
  }

  SpecializationResult recover() const {
    return specializeBinaryInterpreter(Image, Entry, Options);
  }
  BinaryLowIRRefinementResult
  check(const LowFunc &Candidate, Witness W = Witness::LiftedBits,
        const LowIRRefinementLimits &Limits = {}) const {
    return checkBinaryLowIRRefinement(Image, Entry, Options, Candidate,
                                      Contract, W, Limits);
  }
};

void refused(const BinaryLowIRRefinementResult &Result, Status S) {
  EXPECT_EQ(Result.Proof.Status, S) << Result.Proof.Diagnostic;
  EXPECT_FALSE(Result.proved());
  EXPECT_FALSE(Result.Certificate);
  EXPECT_FALSE(Result.Proof.Certificate);
}

Program guardedNativeChain(unsigned Count, bool Taken) {
  Program P({});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 8};
  auto &Code = P.Image.Segments.front();
  const auto Immediate = [&](uint32_t Value) {
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Value >> (8 * I)));
  };
  for (unsigned I = 0; I != Count; ++I) {
    // LEA RAX,[RSP+bias]; AND EAX,127; CMP EAX,entry-residue+bias.
    const uint8_t Bias = I + 1, Expected = (8 + Bias) & 127;
    Code.Data.insert(Code.Data.end(), {0x48, 0x8d, 0x44, 0x24, Bias, 0x83, 0xe0,
                                       127, 0x83, 0xf8, Expected, 0x0f});
    Code.Data.push_back(Taken ? 0x84 : 0x85);
    if (Taken) {
      Immediate(1); // JE next skips a trapping fallthrough.
      Code.Data.push_back(0xcc);
    } else {
      // JNE trap, after the final MOV EAX,7; RET.
      Immediate(Count * 17 + 6 - (Code.Data.size() + 4));
    }
  }
  Code.Data.insert(Code.Data.end(), {0xb8, 7, 0, 0, 0, 0xc3, 0xcc});
  Code.Size = Code.FileSz = Code.Data.size();
  return P;
}

TEST(BinaryLowIRRefinement, ProvedNativeBranchChoiceKeepsTheIncomingDomain) {
  for (bool Taken : {false, true}) {
    SCOPED_TRACE(Taken);
    auto P = guardedNativeChain(32, Taken);
    auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    LowIRRefinementLimits Limits;
    Limits.Execution.Solver.Blast.MaxGates = 512;
    const auto Good = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_EQ(Good.Proof.OriginalPaths, 1U);
    EXPECT_EQ(Good.Proof.CandidatePaths, 1U);
    EXPECT_EQ(Good.Proof.TerminalPairs, 1U);
    ASSERT_GT(Good.Proof.SolverQueries, 0U);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    ASSERT_TRUE(
        P.check(Recovery.Residual, Witness::LiftedBits, Limits).proved());
    --Limits.Execution.MaxSolverQueries;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    Limits.Execution.Solver.Blast.MaxGates = 1;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.Solver.Blast.MaxGates = 512;

    P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 9};
    refused(P.check(Recovery.Residual), Status::ContractViolation);
    P.Options.EntryFrameAlignment.reset();
    P.Contract.Frame->EntryAlignment.reset();
    refused(P.check(Recovery.Residual), Status::ContractViolation);
    P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 8};
    P.Image.Segments.front().Data[10] ^= 1;
    refused(P.check(Recovery.Residual), Status::ContractViolation);
    P.Image.Segments.front().Data[10] ^= 1;

    bool Changed = false;
    for (auto &B : Recovery.Residual.Blocks)
      for (auto &O : B.Ops)
        if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
            O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
            O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
          O.Inputs[0].Offset = 9;
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    // A different terminal observation must still be checked after coverage.
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::Different);
  }
}

TEST(BinaryLowIRRefinement, SingletonTargetsRetainTheirBranchDomains) {
  // TEST EDI,1; JNZ odd. Each arm computes a distinct singleton target from
  // the symbolic entry stack, then returns a distinct ECX value.
  Program P({0xf7, 0xc7, 1, 0, 0, 0, 0x75, 16});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
  auto &Code = P.Image.Segments.front();
  for (uint8_t Bias : {1, 2}) {
    const uint32_t Target = Entry + 40 + (Bias - 1) * 6;
    const uint32_t Base = Target - (8 + Bias);
    Code.Data.insert(Code.Data.end(), {0x48, 0x8d, 0x44, 0x24, Bias, 0x83, 0xe0,
                                       15, 0x48, 0x05});
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Base >> (8 * I)));
    Code.Data.insert(Code.Data.end(), {0xff, 0xe0});
  }
  Code.Data.insert(Code.Data.end(),
                   {0xb9, 7, 0, 0, 0, 0xc3, 0xb9, 9, 0, 0, 0, 0xc3});
  Code.Size = Code.FileSz = Code.Data.size();
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxIndirectTargets = 1;
  const auto Good = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 2U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 2U);
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RCX &&
          O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
          Status::Different);
}

TEST(BinaryLowIRRefinement, MemoryIndirectDispatchKeepsTargetSnapshot) {
  // AND ECX,3; LEA RAX,[RIP+table]; JMP [RAX+RCX*8]; four return arms.
  Program P({0x83, 0xe1, 3, 0x48, 0x8d, 0x05, 27,   0, 0, 0, 0xff, 0x24, 0xc8,
             0xb8, 7,    0, 0,    0,    0xc3, 0xb8, 9, 0, 0, 0,    0xc3, 0xb8,
             11,   0,    0, 0,    0xc3, 0xb8, 13,   0, 0, 0, 0xc3});
  auto &Code = P.Image.Segments.front();
  ASSERT_EQ(Code.Data.size(), 37U);
  for (uint64_t Target : {Entry + 13, Entry + 19, Entry + 25, Entry + 31})
    for (unsigned I = 0; I != 8; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Target >> (I * 8)));
  Code.Size = Code.FileSz = Code.Data.size();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 4U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 4U);
  auto Changed = Recovery.Residual;
  ASSERT_EQ(Changed.FunctionTemporaries.size(), 1U);
  bool Mutated = false;
  for (auto &B : Changed.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::COPY && O.Output.isTemp() &&
          O.Output.Offset == Changed.FunctionTemporaries[0].Offset) {
        O.Inputs[0] = NdVar::scalar(Entry + 13, 8);
        Mutated = true;
      }
  ASSERT_TRUE(Mutated);
  refused(P.check(Changed), Status::Different);
  Changed = Recovery.Residual;
  Changed.FunctionTemporaries.clear();
  refused(P.check(Changed), Status::Invalid);
}

TEST(BinaryLowIRRefinement, FiniteReturnDispatchKeepsPreCleanupTarget) {
  // Reserve a cleanup slot and call a helper. It replaces its return slot
  // with one of four arms, then RET 8 restores the outer stack pointer.
  Program P({0x48, 0x83, 0xec, 8,    0xe8, 24,   0,    0,    0,    0xb8,
             7,    0,    0,    0,    0xc3, 0xb8, 9,    0,    0,    0,
             0xc3, 0xb8, 11,   0,    0,    0,    0xc3, 0xb8, 13,   0,
             0,    0,    0xc3, 0x83, 0xe1, 3,    0x48, 0x8d, 0x05, 0xde,
             0xff, 0xff, 0xff, 0x48, 0x8d, 0x0c, 0x49, 0x48, 0x8d, 0x04,
             0x48, 0x48, 0x89, 0x04, 0x24, 0xc2, 8,    0});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 4U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 4U);
}

TEST(BinaryLowIRRefinement, FiniteMemoryCallKeepsOverwrittenTargetSlot) {
  // Select a callee into [RSP-8]. CALL reads it before pushing the return
  // address into that same slot, so a later reload is not a target snapshot.
  Program P({0x83, 0xe1, 1,    0x48, 0x8d, 0x05, 18,   0,    0,    0,
             0x48, 0x8d, 0x0c, 0x49, 0x48, 0x8d, 0x04, 0x48, 0x48, 0x89,
             0x44, 0x24, 0xf8, 0xff, 0x54, 0x24, 0xf8, 0xc3, 0xb8, 7,
             0,    0,    0,    0xc3, 0xb8, 9,    0,    0,    0,    0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 2U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 2U);
}

TEST(BinaryLowIRRefinement,
     Disp32WordShiftChecksTheCompleteRecoveredCandidate) {
  Program P({0x48, 0x89, 0x4c, 0x24, 0xf8, 0x66, 0xc1, 0xa4, 0x24, 0xf8,
             0xff, 0xff, 0xff, 3,    0x0f, 0xb7, 0x44, 0x24, 0xf8, 0xc3});
  auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Good = P.check(R.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : R.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INT_LEFT && O.Output.Size == 2 &&
          O.NumInputs == 2 && O.Inputs[1] == NdVar::scalar(3, 2)) {
        O.Inputs[1] = NdVar::scalar(2, 2);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(R.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, PhysicalReturnDestinationsKeepFullStateRelation) {
  // The helper adjusts its return slot to skip two invalid inline bytes.
  Program P({0xe8, 8, 0, 0, 0, 0x16, 0x06, 0xb8, 7, 0, 0, 0, 0xc3, 0x48, 0x83,
             0x04, 0x24, 2, 0xc3});
  auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Good = P.check(R.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : R.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
          O.NumInputs == 1 && O.Inputs[0].isConst() &&
          O.Inputs[0].Offset == 7) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(R.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, DeferredConditionalEdgesKeepFullStateRelation) {
  // CMP EAX,EAX; JE suffix; invalid bytes; suffix: MOV EAX,7; RET.
  Program P({0x39, 0xc0, 0x74, 2, 0x16, 0x06, 0xb8, 7, 0, 0, 0, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  P.Contract.DeferNativeConditionalEdges = true;
  for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
    const auto Good = P.check(Recovery.Residual, W);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(
        Good.Certificate->Relation.Contract.DeferNativeConditionalEdges);
    EXPECT_EQ(Good.Certificate->Instructions.size(), 4U);
    EXPECT_TRUE(Good.Certificate->Relation.NativeAuditBoundaries.empty());
  }
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
          O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, DeferredEdgesRespectSelectedUndefinedWitness) {
  // ADD establishes OF=1; BT makes it arbitrary, keeping the old lifted bit.
  // Only ZeroBits takes JNO into the invalid byte. Independence must refuse.
  Program P({0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1, 0x0f, 0xa3,
             0xc8, 0x71, 6,    0xb8, 7,    0,    0,    0, 0xc3, 0x16});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Unsupported);
  const auto Independent =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(Independent.Proof.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Independent.Certificate);
}

TEST(BinaryLowIRRefinement, DeferredEdgePolicyKeepsDigestsAndPlanValidation) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete());
  const auto Strict = P.check(Recovery.Residual);
  ASSERT_TRUE(Strict.proved());
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Deferred = P.check(Recovery.Residual);
  ASSERT_TRUE(Deferred.proved()) << Deferred.Proof.Diagnostic;
  EXPECT_NE(Strict.Certificate->Relation.OriginalDigest,
            Deferred.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Strict.Certificate->Relation.InputDigest,
            Deferred.Certificate->Relation.InputDigest);
  EXPECT_NE(Strict.Certificate->InputDigest, Deferred.Certificate->InputDigest);
  const auto Loop = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery.Residual, P.Contract, {});
  refused(Loop, Status::Invalid);
  EXPECT_NE(Loop.Proof.Diagnostic.find("nonempty cutpoint plan"),
            std::string::npos);
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Inferred.Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Inference.Plan);
  refused(Inferred.Refinement, Status::Unsupported);
}

TEST(BinaryLowIRRefinement, SplitAlignedFrameStoresKeepTheFullStateRelation) {
  Program P({0x48, 0x8d, 0x54, 0x24, 0xbf, 0x80, 0xe2, 0xf0, 0x48, 0x8d, 0x7a,
             9, 0x89, 0x0f, 0x8b, 0x07, 0xc3});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::STORE && O.NumInputs == 2) {
        O.Inputs[1] = NdVar::scalar(0, 4);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, RepeatedFeasibilityKeepsCandidateStateChecks) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  auto &Code = P.Image.Segments.front();
  Code.Data.insert(Code.Data.end() - 1, 128, 0x90);
  Code.Size = Code.FileSz = Code.Data.size();
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
          O.NumInputs == 1 && O.Inputs[0].isConst() &&
          O.Inputs[0].Offset == 7) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, EntryAlignmentUsesTheOriginalSymbolicStack) {
  Program P({0x48, 0x89, 0xe0, 0xc3}); // mov rax,rsp; ret.
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  // OR preserves the declared low bits but cannot discard higher root bits.
  bool Changed = false;
  for (auto &Block : Recovery.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY && Op.Output == NdVar::reg(x86reg::RAX, 8) &&
          Op.Inputs[0] == NdVar::reg(x86reg::RSP, 8)) {
        Op.Opcode = NdOp::INT_OR;
        Op.addInput(NdVar::scalar(3, 8));
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment =
      InterpreterEntryAlignment{16, 3};
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Contract.Frame->EntryAlignment,
            P.Options.EntryFrameAlignment);
  P.Options.EntryFrameAlignment->Residue = 4;
  P.Contract.Frame->EntryAlignment = P.Options.EntryFrameAlignment;
  refused(P.check(Recovery.Residual), Status::Different);
  P.Options.EntryFrameAlignment->Residue = 3;
  P.Contract.Frame->EntryAlignment = P.Options.EntryFrameAlignment;
  for (auto &Block : Recovery.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INT_OR && Op.Output == NdVar::reg(x86reg::RAX, 8))
        Op.Inputs[1] = NdVar::scalar(35, 8);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, EntryAlignmentRequiresMatchingValidContracts) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete());
  P.Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 3};
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Contract.Frame->EntryAlignment = {16, 4};
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Contract.Frame->EntryAlignment = P.Options.EntryFrameAlignment;
  ASSERT_TRUE(P.check(Recovery.Residual).proved());
  const auto Independent =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  ASSERT_TRUE(Independent.proved()) << Independent.Proof.Diagnostic;
  EXPECT_EQ(Independent.Certificate->LowIR.Contract.Frame->EntryAlignment,
            P.Options.EntryFrameAlignment);
  P.Options.EntryFrameAlignment.reset();
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment;
  P.Options.FrameBaseRegister->Offset = x86reg::RAX;
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.FrameBaseRegister.reset();
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment =
      InterpreterEntryAlignment{3, 1};
  refused(P.check(Recovery.Residual), Status::Invalid);
}

TEST(BinaryLowIRRefinement, ActualResidualAndOriginalBytesAreBound) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret.
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Scope,
            LowIRRefinementScope::CompleteFiniteNativeToLowIRPaths);
  ASSERT_EQ(Good.Certificate->Instructions.size(), 2U);
  EXPECT_EQ(Good.Certificate->Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0xb8, 7, 0, 0, 0}));
  P.Image.Segments[0].Data[1] = 9;
  refused(P.check(Recovery.Residual), Status::Different);
  EXPECT_EQ(Good.Certificate->Instructions[0].NativeBytes[1], 7);
}

TEST(BinaryLowIRRefinement, RegisterCaseDispatchChecksBothNativeControlCases) {
  // mov eax,16; mov edx,32; test cl,1; cmovz rax,rdx; jmp body;
  // body: mov r8,rsp; sub r8,rax; mov byte ptr [r8],90; ret.
  Program P({0xb8, 16,   0,    0,    0,    0xba, 32,   0,    0,  0,
             0xf6, 0xc1, 1,    0x48, 0x0f, 0x44, 0xc2, 0xeb, 0,  0x49,
             0x89, 0xe0, 0x49, 0x29, 0xc0, 0x41, 0xc6, 0,    90, 0xc3});
  P.Options.ControlRegisters = {{x86reg::RAX, 8}};
  P.Options.DiscoverControlState = true;
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  EXPECT_GT(Recovery.ControlRefinements, 0u);
  // The native memory checker currently requires a unique frame offset at
  // every access. Bind each selector separately here; the recovery above
  // and source runtime regression keep the selector unconstrained.
  for (uint64_t Input : {0, 1}) {
    P.Options.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    P.Contract.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    const auto Good = P.check(Recovery.Residual);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(Good.Certificate->Relation.Contract.ObserveWrittenFrameBytes);
  }
  // A deliberately misrouted case must be rejected by the independent
  // native comparison, even if the residual graph itself remains well formed.
  bool Changed = false;
  for (auto &Block : Recovery.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INT_EQUAL && Op.Inputs[0].isReg() &&
          Op.Inputs[0].Offset == x86reg::RAX && Op.Inputs[0].Size == 8 &&
          Op.Inputs[1].isConst()) {
        ++Op.Inputs[1].Offset;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  bool Rejected = false;
  for (uint64_t Input : {0, 1}) {
    P.Options.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    P.Contract.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    const auto Bad = P.check(Recovery.Residual);
    if (Bad.Proof.Status == Status::Different) {
      refused(Bad, Status::Different);
      Rejected = true;
    }
  }
  EXPECT_TRUE(Rejected);
}

TEST(BinaryLowIRRefinement, OverlappingEntriesKeepBothFeasibleBranchResults) {
  for (uint8_t Branch : {0x74, 0x75}) {
    // TEST ECX,ECX; JZ/JNZ second_mov; MOV EAX,0x7b8; RET; RET.
    // second_mov starts inside the first MOV and returns 0xc3000007.
    Program P({0x85, 0xc9, Branch, 1, 0xb8, 0xb8, 7, 0, 0, 0xc3, 0xc3});
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    refused(P.check(Recovery.Residual), Status::Unsupported);
    P.Contract.AllowOverlappingNativeInstructions = true;
    const auto Good = P.check(Recovery.Residual);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_EQ(Good.Certificate->Instructions.size(), 6u);
    EXPECT_EQ(Good.Proof.OriginalPaths, 2u);
    EXPECT_EQ(Good.Proof.CandidatePaths, 2u);
    EXPECT_TRUE(
        Good.Certificate->Relation.Contract.AllowOverlappingNativeInstructions);
    for (uint64_t Value : {UINT64_C(0x7b8), UINT64_C(0xc3000007)}) {
      auto Changed = Recovery.Residual;
      bool Found = false;
      for (auto &Block : Changed.Blocks)
        for (auto &Op : Block.Ops)
          // Recovery can propagate the immediate through later zero-extends.
          // Change every data use for just this arm, not only a dead COPY.
          if (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT)
            for (unsigned I = 0; I != Op.NumInputs; ++I)
              if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Value) {
                ++Op.Inputs[I].Offset;
                Found = true;
              }
      ASSERT_TRUE(Found);
      refused(P.check(Changed), Status::Different);
    }
    // The shared byte changes both native interpretations. Neither branch
    // can be ignored merely because its entry is inside another instruction.
    P.Image.Segments.front().Data[6] = 8;
    refused(P.check(Recovery.Residual), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, NativeOverlapOptionBindsDigestsAndRejectsLoopAPIs) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Strict = P.check(Recovery.Residual);
  ASSERT_TRUE(Strict.proved()) << Strict.Proof.Diagnostic;
  P.Contract.AllowOverlappingNativeInstructions = true;
  const auto Finite = P.check(Recovery.Residual);
  ASSERT_TRUE(Finite.proved()) << Finite.Proof.Diagnostic;
  EXPECT_NE(Strict.Certificate->Relation.OriginalDigest,
            Finite.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Strict.Certificate->Relation.InputDigest,
            Finite.Certificate->Relation.InputDigest);
  EXPECT_NE(Strict.Certificate->InputDigest, Finite.Certificate->InputDigest);
  LowIRLoopRefinementPlan Plan;
  refused(checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                         Recovery.Residual, P.Contract, Plan),
          Status::Unsupported);
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Inferred.Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Inference.Plan);
  refused(Inferred.Refinement, Status::Unsupported);
}

TEST(BinaryLowIRRefinement, UndefinedCopiesAndStackRemainObservable) {
  // xor eax,eax; pushfq; pop rcx; mov rdx,rcx; ret. AF remains observable in
  // both copied registers, the flags bank and the written push slot.
  Program P({0x31, 0xc0, 0x9c, 0x59, 0x48, 0x89, 0xca, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_FALSE(Good.Certificate->Relation.Producers.empty());
  EXPECT_EQ(Good.Certificate->Relation.Witness, Witness::LiftedBits);
  const auto Strict =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(Strict.Proof.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Strict.Certificate);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::COPY && O.Output == NdVar::reg(x86reg::RDX, 8)) {
        O.Inputs[0] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, GuardedShiftSupportsAllSymbolicCounts) {
  // shl eax,cl; pushfq; pop rdx; ret. Zero count preserves flags; the witness
  // may select AF/OF only when their audited instruction guards activate.
  Program P({0xd3, 0xe0, 0x9c, 0x5a, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_GE(Good.Certificate->Relation.Producers.size(), 2U);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
}

TEST(BinaryLowIRRefinement, UnreachableBoundaryDoesNotHideReachableSuffix) {
  // CMP EAX,EAX; JE suffix; RCL EDX,1; suffix: MOV EAX,7; RET.
  // Collection sees the dead fallthrough boundary before the taken suffix.
  Program P({0x39, 0xc0, 0x74, 2, 0xd1, 0xd2, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  P.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Certificate->Relation.NativeAuditBoundaries.size(), 1u);
  EXPECT_EQ(
      Good.Certificate->Relation.NativeAuditBoundaries.front().Boundary.Address,
      Entry + 4);
  EXPECT_TRUE(
      std::any_of(Good.Certificate->Instructions.begin(),
                  Good.Certificate->Instructions.end(),
                  [](const auto &I) { return I.Origin.Address == Entry + 6; }));
  // RCR EDX,1 is another valid, equally sized instruction with Missing
  // coverage. Its unreachable bytes still bind all three proof digests.
  P.Image.Segments.front().Data[5] = 0xda;
  const auto ChangedBoundary = P.check(Recovery.Residual);
  ASSERT_TRUE(ChangedBoundary.proved()) << ChangedBoundary.Proof.Diagnostic;
  EXPECT_NE(Good.Certificate->Relation.OriginalDigest,
            ChangedBoundary.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Good.Certificate->Relation.InputDigest,
            ChangedBoundary.Certificate->Relation.InputDigest);
  EXPECT_NE(Good.Certificate->InputDigest,
            ChangedBoundary.Certificate->InputDigest);
  P.Image.Segments.front().Data[7] = 8;
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, BoundaryUnreachabilityIsRelativeToSelectedWitness) {
  // ADD establishes OF=1; BT makes it arbitrary while its ordinary lift keeps
  // the old bit. LiftedBits cannot take JNO, but ZeroBits does take it.
  Program P({0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1,    0x0f, 0xa3, 0xc8,
             0x71, 6,    0xb8, 7,    0,    0,    0,    0xc3, 0xd1, 0xd2, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Certificate->Relation.NativeAuditBoundaries.size(), 1u);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Unsupported);
  const auto Independent =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(Independent.Proof.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Independent.Certificate);
}

TEST(BinaryLowIRRefinement, BoundaryPolicyKeepsDigestsAndPlanValidation) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Strict = P.check(Recovery.Residual);
  ASSERT_TRUE(Strict.proved()) << Strict.Proof.Diagnostic;
  P.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Finite = P.check(Recovery.Residual);
  ASSERT_TRUE(Finite.proved()) << Finite.Proof.Diagnostic;
  EXPECT_TRUE(Finite.Certificate->Relation.NativeAuditBoundaries.empty());
  EXPECT_NE(Strict.Certificate->Relation.OriginalDigest,
            Finite.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Strict.Certificate->Relation.InputDigest,
            Finite.Certificate->Relation.InputDigest);
  EXPECT_NE(Strict.Certificate->InputDigest, Finite.Certificate->InputDigest);
  LowIRLoopRefinementPlan Plan;
  const auto Loop = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan);
  refused(Loop, Status::Invalid);
  EXPECT_NE(Loop.Proof.Diagnostic.find("nonempty cutpoint plan"),
            std::string::npos);
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Inferred.Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Inference.Plan);
  refused(Inferred.Refinement, Status::Unsupported);
}

TEST(BinaryLowIRRefinement, XaddHasNoFreshBitsAndRejectsAlteredDefinedOutputs) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0xc0, 0xc4},
                                         {0x0f, 0xc0, 0xe0},
                                         {0x0f, 0xc1, 0xc8},
                                         {0x48, 0x0f, 0xc1, 0xc0},
                                         {0x4d, 0x0f, 0xc1, 0xc8}}) {
    Program P({});
    auto &Code = P.Image.Segments.front();
    Code.Data = Bytes;
    Code.Data.push_back(0xc3);
    Code.Size = Code.FileSz = Code.Data.size();
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
      const auto Good = P.check(Recovery.Residual, W);
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
      EXPECT_TRUE(Good.Certificate->Relation.Producers.empty());
    }
  }
  Program P({0x0f, 0xc1, 0xc8, 0xc3}); // XADD EAX,ECX; RET.
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  for (unsigned Field = 0; Field != 3; ++Field) {
    auto Changed = Recovery.Residual;
    bool Found = false;
    for (auto &Block : Changed.Blocks)
      for (auto &Op : Block.Ops)
        if (!Found && (Field == 0   ? Op.Opcode == NdOp::INT_ADD
                       : Field == 1 ? Op.Output == NdVar::reg(x86reg::RCX, 4)
                                    : Op.Output == NdVar::reg(x86reg::CF, 1))) {
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
          Op.Inputs[0] = NdVar::scalar(0, Op.Output.Size);
          Found = true;
        }
    ASSERT_TRUE(Found);
    refused(P.check(Changed), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, MemoryXaddBindsSumAddressSourceAndFlags) {
  // LEA RCX,[RSP-16]; XADD [RCX],ECX; RET. Writing ECX zero-extends RCX,
  // while the memory update must use the complete original address.
  Program P({0x48, 0x8d, 0x4c, 0x24, 0xf0, 0x0f, 0xc1, 0x09, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
    const auto Good = P.check(Recovery.Residual, W);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(Good.Certificate->Relation.Producers.empty());
  }
  for (unsigned Field = 0; Field != 4; ++Field) {
    auto Changed = Recovery.Residual;
    bool Found = false;
    for (auto &Block : Changed.Blocks)
      for (auto &Op : Block.Ops) {
        if (Found)
          continue;
        if (Field < 2 && Op.Opcode == NdOp::STORE) {
          if (Field == 0)
            Op.Inputs[1] = NdVar::scalar(0, Op.Inputs[1].Size);
          else
            Op.Inputs[0] = NdVar::reg(x86reg::RSP, 8);
          Found = true;
        } else if ((Field == 2 && Op.Output.isReg() &&
                    Op.Output.Offset == x86reg::RCX && Op.Output.Size == 4) ||
                   (Field == 3 && Op.Output == NdVar::reg(x86reg::CF, 1))) {
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
          Op.Inputs[0] = NdVar::scalar(0, Op.Output.Size);
          Found = true;
        }
      }
    ASSERT_TRUE(Found) << Field;
    const auto Bad = P.check(Changed);
    refused(Bad, Field == 1 ? Status::ContractViolation : Status::Different);
  }
}

TEST(BinaryLowIRRefinement, DoubleShiftWitnessesBindArbitraryAndDefinedSlices) {
  for (bool Right : {false, true})
    for (const auto &Prefix :
         std::vector<std::vector<uint8_t>>{{0x66}, {}, {0x48}}) {
      Program P({});
      auto &Code = P.Image.Segments.front();
      Code.Data = Prefix;
      // Destination RCX aliases CL; count must remain the entry low byte.
      Code.Data.insert(Code.Data.end(),
                       {0x0f, uint8_t(Right ? 0xad : 0xa5), 0xd1, 0xc3});
      Code.Size = Code.FileSz = Code.Data.size();
      const auto R = P.recover();
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      const auto Good = P.check(R.Residual);
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
      EXPECT_FALSE(Good.Certificate->Relation.Producers.empty());
      refused(P.check(R.Residual, Witness::ZeroBits), Status::Different);
    }
  Program P({0x66, 0x0f, 0xa4, 0xd0, 16, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(P.check(R.Residual).proved());
  // Count exactly 16 defines the low word and CF.
  for (unsigned Field = 0; Field != 3; ++Field) {
    auto Candidate = R.Residual;
    bool Found = false;
    for (auto &B : Candidate.Blocks)
      for (auto &O : B.Ops)
        if (!Found && (Field == 0   ? O.Output == NdVar::reg(x86reg::RAX, 2)
                       : Field == 1 ? O.Output == NdVar::reg(x86reg::CF, 1)
                                    : O.Output == NdVar::reg(x86reg::RAX, 2))) {
          O.Opcode = NdOp::COPY;
          O.NumInputs = 1;
          if (Field == 2)
            O.Output = NdVar::reg(x86reg::RAX, 8);
          O.Inputs[0] = NdVar::scalar(0, O.Output.Size);
          Found = true;
        }
    ASSERT_TRUE(Found);
    refused(P.check(Candidate), Status::Different);
  }
  Program Excess({0x66, 0x0f, 0xa4, 0xd0, 17, 0xc3});
  auto ExcessRecovery = Excess.recover();
  ASSERT_TRUE(ExcessRecovery.complete()) << ExcessRecovery.Diagnostic;
  ASSERT_TRUE(Excess.check(ExcessRecovery.Residual).proved());
  bool Found = false;
  for (auto &B : ExcessRecovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Output == NdVar::reg(x86reg::RAX, 2)) {
        // This selected low-word witness is already zero. Clearing the
        // remaining register bits still changes architecturally defined state.
        O.Opcode = NdOp::COPY;
        O.Output = NdVar::reg(x86reg::RAX, 8);
        O.NumInputs = 1;
        O.Inputs[0] = NdVar::scalar(0, 8);
        Found = true;
      }
  ASSERT_TRUE(Found);
  refused(Excess.check(ExcessRecovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, CompatibilityShiftUsesTheSameSelectedValueWitness) {
  Program P(
      {0xd3, 0xf0, 0x9c, 0x5a, 0xc3}); // SAL /6 EAX,CL; PUSHFQ; POP RDX; RET.
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_GE(Good.Certificate->Relation.Producers.size(), 2U);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
}

TEST(BinaryLowIRRefinement,
     BitTestsAndOverlappingRotatesKeepFullStateWitnesses) {
  const std::vector<std::vector<uint8_t>> Instructions = {
      {0x0f, 0xa3, 0xc8}, {0x0f, 0xab, 0xc8}, {0x0f, 0xb3, 0xc8},
      {0x0f, 0xbb, 0xc8}, {0xd2, 0xc1},       {0xd2, 0xc9}};
  for (const auto &Bytes : Instructions) {
    Program P({});
    auto &Code = P.Image.Segments.front();
    Code.Data = Bytes;
    Code.Data.insert(Code.Data.end(),
                     {0x9c, 0x5a, 0xc3}); // PUSHFQ; POP RDX; RET.
    Code.Size = Code.FileSz = Code.Data.size();
    auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    const auto Good = P.check(Recovery.Residual);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_FALSE(Good.Certificate->Relation.Producers.empty());
    refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
    bool Changed = false;
    for (auto &B : Recovery.Residual.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::COPY && O.Output == NdVar::reg(x86reg::RDX, 8)) {
          O.Inputs[0] = NdVar::scalar(0, 8);
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    refused(P.check(Recovery.Residual), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, PhysicalCallAndModifiedReturnTarget) {
  // call body; nop; ret; body: inc qword [rsp]; ret. The modified continuation
  // skips the NOP. Original execution must load the actual stack target.
  Program P({0xe8, 2, 0, 0, 0, 0x90, 0xc3, 0x48, 0xff, 0x04, 0x24, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_TRUE(Good.Certificate->Relation.Contract.ObserveWrittenFrameBytes);
}

TEST(BinaryLowIRRefinement, FiniteLoopsCheckEveryInputPath) {
  // mov ecx,edi; and ecx,3; mov eax,0; test ecx,ecx; jz done;
  // again: inc eax; dec ecx; jnz again; done: ret.
  Program P({0x89, 0xf9, 0x83, 0xe1, 3,    0xb8, 0,    0,    0,    0,   0x85,
             0xc9, 0x74, 6,    0xff, 0xc0, 0xff, 0xc9, 0x75, 0xfa, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 4U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 4U);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxBlockVisits = Good.Proof.BlockVisits - 1;
  refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
          Status::BudgetExceeded);
}

TEST(BinaryLowIRLoopRefinement, UnboundedInputCountAndOriginalBytes) {
  // top: jrcxz done; add rax,rcx;
  // lea rcx,[rcx-1]; jmp top; done: ret. No finite unrolling can cover all
  // uint64 input counts. The rank excludes wraparound using the JRCXZ guard.
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry;
  Cut.UseEntryPrefix = true;
  unsigned Mappings = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Mappings;
    }
  ASSERT_EQ(Mappings, 1U);
  const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
    const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
    const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
    Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
    Cut.OriginalState.push_back({L, T});
    Cut.CandidateState.push_back({L, T});
    return T;
  };
  Parameter(x86reg::RAX, 8);
  Cut.Rank = {Parameter(x86reg::RCX, 8)};
  for (auto Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Parameter(Flag, 1);
  LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Proof.LoopInitiations, 1U);
  EXPECT_EQ(Good.Proof.RankingChecks, 1U);
  EXPECT_LT(Good.Proof.Instructions, 30U);
  // A changed native decrement must be re-executed, not matched only by PC.
  P.Image.Segments[0].Data[8] = 0xfe;
  refused(Check(), Status::Different);
}

TEST(BinaryLowIRLoopRefinement, CandidateTemporaryRetainsItsRealEntryValue) {
  // top: jrcxz done; add rax,rdx; lea rcx,[rcx-1]; jmp top; done: ret.
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xd0, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  auto &Candidate = Recovery.Residual;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry;
  Cut.UseEntryPrefix = true;
  unsigned Mappings = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Entry) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Mappings;
    }
  ASSERT_EQ(Mappings, 1U);
  const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
    const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
    const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
    Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
    Cut.OriginalState.push_back({L, T});
    Cut.CandidateState.push_back({L, T});
    return T;
  };
  Parameter(x86reg::RAX, 8);
  Cut.Rank = {Parameter(x86reg::RCX, 8)};
  for (auto Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Parameter(Flag, 1);

  constexpr uint64_t Saved = uint64_t{1} << 60;
  constexpr va_t PrefixAddress = 0x80000000;
  LowBlock Prefix;
  Prefix.Id = -1;
  unsigned Replaced = 0;
  for (auto &B : Candidate.Blocks) {
    ASSERT_NE(B.StartAddr, PrefixAddress);
    Prefix.Id = std::max(Prefix.Id, B.Id);
    if (B.StartAddr == Candidate.Entry)
      Prefix.Succs = {B.Id};
    for (auto &O : B.Ops)
      for (unsigned I = 0; I != O.NumInputs; ++I)
        if (O.Inputs[I] == NdVar::reg(x86reg::RDX, 8)) {
          O.Inputs[I] = NdVar::tmp(Saved, 8);
          ++Replaced;
        }
  }
  ASSERT_GT(Replaced, 0U);
  ASSERT_EQ(Prefix.Succs.size(), 1U);
  ++Prefix.Id;
  Prefix.StartAddr = PrefixAddress;
  Prefix.EndAddr = PrefixAddress + 1;
  LowOp Save;
  Save.Opcode = NdOp::COPY;
  Save.Output = NdVar::tmp(Saved, 8);
  Save.addInput(NdVar::reg(x86reg::RDX, 8));
  Save.Addr = PrefixAddress;
  Save.Seq = 0;
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.addInput(NdVar::scalar(Candidate.Entry, 8));
  Branch.Addr = PrefixAddress;
  Branch.Seq = 1;
  Prefix.Ops = {Save, Branch};
  LowInstructionBoundary Boundary;
  Boundary.Address = PrefixAddress;
  Boundary.Size = 1;
  Boundary.OpCount = 2;
  Boundary.Control = LowInstructionControl::Branch;
  Boundary.ControlFlags = LowInstructionControlFlag::Branch;
  Boundary.Immediate = Candidate.Entry;
  Prefix.InstructionBoundaries = {Boundary};
  for (auto *Roots : {&Candidate.ModuleAnalysisRoots,
                      &Candidate.OrdinaryModuleAnalysisRoots}) {
    for (va_t Root : *Roots)
      ASSERT_EQ(Root, Candidate.Entry);
    if (!Roots->empty())
      *Roots = {PrefixAddress};
  }
  Candidate.Entry = PrefixAddress;
  Candidate.FunctionTemporaries = {{Saved, 8}};
  Candidate.Blocks.push_back(std::move(Prefix));
  const LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options, Candidate,
                                          P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Proof.RankingChecks, 1U);
  Candidate.Blocks.back().Ops.front().Inputs[0] = NdVar::reg(x86reg::R8, 8);
  refused(Check(), Status::Different);
  Candidate.Blocks.back().Ops.front().Inputs[0] = NdVar::reg(x86reg::RDX, 8);
  Candidate.FunctionTemporaries.clear();
  refused(Check(), Status::Invalid);
}

TEST(BinaryLowIRLoopInference, ChangingTemporaryStateBelongsOnlyToCandidate) {
  // nop; top: jrcxz done; add rax,rcx; lea rcx,[rcx-1]; jmp top; done: ret.
  // The candidate maintains an additional private counter. Its changing bits
  // still need an exact template, even though the native program has no slot.
  Program P({0x90, 0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb,
             0xf5, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  constexpr uint64_t Saved = uint64_t{1} << 60;
  Recovery.Residual.FunctionTemporaries = {{Saved, 8}};
  unsigned Initialized = 0, Updated = 0;
  for (auto &B : Recovery.Residual.Blocks) {
    auto Previous = B.Ops;
    B.Ops.clear();
    for (auto &Boundary : B.InstructionBoundaries) {
      const auto Origin = std::find_if(
          Recovery.Origins.begin(), Recovery.Origins.end(),
          [&](const auto &O) { return O.ResidualAddress == Boundary.Address; });
      const auto Begin = Boundary.FirstOp;
      Boundary.FirstOp = B.Ops.size();
      B.Ops.insert(B.Ops.end(), Previous.begin() + Begin,
                   Previous.begin() + Begin + Boundary.OpCount);
      if (Origin == Recovery.Origins.end())
        continue; // keep synthetic recovery control boundaries unchanged
      const auto Native = Origin->NativeInstruction.Address;
      if (Native != Entry && Native != Entry + 6)
        continue;
      LowOp Update;
      Update.Addr = Boundary.Address;
      Update.Seq = Boundary.OpCount++;
      if (Native == Entry) {
        Update.Opcode = NdOp::COPY;
        Update.Output = NdVar::tmp(Saved, 8);
        Update.addInput(NdVar::scalar(0, 8));
        ++Initialized;
      } else {
        Update.Opcode = NdOp::INT_ADD;
        Update.Output = NdVar::tmp(Saved, 3);
        Update.addInput(Update.Output);
        Update.addInput(NdVar::scalar(1, 3));
        ++Updated;
      }
      B.Ops.push_back(Update);
    }
  }
  ASSERT_GT(Initialized, 0U);
  ASSERT_GT(Updated, 0U);
  const auto Good = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
  ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
  EXPECT_EQ(Good.Refinement.Certificate->Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  auto Plan = *Good.Inference.Plan;
  bool Parameter = false;
  for (const auto &Cut : Plan.Cutpoints) {
    for (const auto &Input : Cut.Inputs)
      if (Input.Location.Space == LowIRLoopSpace::FunctionTemporary) {
        EXPECT_TRUE(Input.Side == LowIRLoopSide::Candidate ||
                    Input.Side == LowIRLoopSide::CandidatePrefix);
        Parameter |= Input.Side == LowIRLoopSide::Candidate;
      }
    for (const auto &Assignment : Cut.OriginalState)
      EXPECT_NE(Assignment.Location.Space, LowIRLoopSpace::FunctionTemporary);
  }
  EXPECT_TRUE(Parameter);
  const auto Check = [&](const LowIRLoopRefinementPlan &Proposal) {
    return checkBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Proposal);
  };
  // Fixed prefix reads use the same candidate-only namespace, whether or not
  // this particular inference heuristic retains a fixed-bit predicate.
  for (auto &Cut : Plan.Cutpoints) {
    uint64_t Next = 0;
    for (const auto &Input : Cut.Inputs)
      Next = std::max(Next, Input.Temporary.Offset + Input.Temporary.Size);
    for (const auto &Expression : Cut.Expressions)
      Next = std::max(Next, Expression.Output.Offset + Expression.Output.Size);
    Cut.Inputs.push_back({LowIRLoopSide::CandidatePrefix,
                          {LowIRLoopSpace::FunctionTemporary, Saved, 8},
                          NdVar::tmp(Next, 8)});
  }
  ASSERT_TRUE(Check(Plan).proved());
  for (bool Input : {false, true}) {
    auto Bad = Plan;
    for (auto &Cut : Bad.Cutpoints) {
      if (Input) {
        for (auto &I : Cut.Inputs)
          if (I.Location.Space == LowIRLoopSpace::FunctionTemporary)
            I.Side = LowIRLoopSide::Original;
      } else {
        for (const auto &Assignment : Cut.CandidateState)
          if (Assignment.Location.Space == LowIRLoopSpace::FunctionTemporary)
            Cut.OriginalState.push_back(Assignment);
      }
    }
    refused(Check(Bad), Status::Invalid);
  }
  auto Missing = Plan;
  for (auto &Cut : Missing.Cutpoints)
    std::erase_if(Cut.CandidateState, [](const auto &A) {
      return A.Location.Space == LowIRLoopSpace::FunctionTemporary;
    });
  refused(Check(Missing), Status::Different);
  P.Image.Segments[0].Data[9] = 0xfe; // the original now decrements by two
  refused(Check(Plan), Status::Different);
}

TEST(BinaryLowIRLoopRefinement, NativeCollectionChecksEveryInductionDomain) {
  for (bool Deferred : {false, true}) {
    SCOPED_TRACE(Deferred);
    // top: jrcxz done; test rcx,rcx; jne body; bad: RCL EDX,1;
    // body: add rax,rcx; lea rcx,[rcx-1]; jmp top; done: ret.
    Program P({0xe3, 16, 0x48, 0x85, 0xc9, 0x75, 2, 0xd1, 0xd2, 0x48, 0x01,
               0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xee, 0xc3});
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    if (Deferred) {
      // The candidate is an untrusted hint. Mutate the native dead arm after
      // recovery: neither invalid byte may be collected by the proof.
      P.Image.Segments[0].Data[7] = 0x16;
      P.Image.Segments[0].Data[8] = 0x06;
    }
    LowIRLoopCutpoint Cut;
    Cut.OriginalAddress = Entry;
    Cut.UseEntryPrefix = true;
    unsigned Mappings = 0;
    for (const auto &Origin : Recovery.Origins)
      if (Origin.NativeInstruction.Address == Entry) {
        Cut.CandidateAddress = Origin.ResidualAddress;
        ++Mappings;
      }
    ASSERT_EQ(Mappings, 1U);
    const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
      const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
      const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
      Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
      Cut.OriginalState.push_back({L, T});
      Cut.CandidateState.push_back({L, T});
      return T;
    };
    Parameter(x86reg::RAX, 8);
    Cut.Rank = {Parameter(x86reg::RCX, 8)};
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF})
      Parameter(Flag, 1);
    const LowIRLoopRefinementPlan Plan{{Cut}};
    const auto Check = [&](const LowIRRefinementLimits &Limits = {}) {
      return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                            Recovery.Residual, P.Contract, Plan,
                                            Witness::LiftedBits, Limits);
    };
    refused(Check(), Status::Unsupported);
    P.Contract.DeferNativeConditionalEdges = Deferred;
    P.Contract.RetainUnauditedNativeBoundaries = !Deferred;
    const auto Good = Check();
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_EQ(Good.Certificate->Relation.Scope,
              LowIRRefinementScope::InductiveNativeToLowIRLoops);
    EXPECT_EQ(Good.Proof.RankingChecks, 1U);
    EXPECT_LT(Good.Proof.Instructions, 32U);
    EXPECT_EQ(Good.Certificate->Relation.NativeAuditBoundaries.size(),
              Deferred ? 0U : 1U);
    const auto Automatic = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    ASSERT_TRUE(Automatic.Inference.inferred())
        << Automatic.Inference.Diagnostic;
    ASSERT_TRUE(Automatic.proved()) << Automatic.Refinement.Proof.Diagnostic;
    const auto OtherWitness = checkBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan,
        Witness::ZeroBits);
    ASSERT_TRUE(OtherWitness.proved()) << OtherWitness.Proof.Diagnostic;
    if (Deferred) {
      P.Contract.RetainUnauditedNativeBoundaries = true;
      const auto Combined = Check();
      ASSERT_TRUE(Combined.proved()) << Combined.Proof.Diagnostic;
      EXPECT_TRUE(Combined.Certificate->Relation.NativeAuditBoundaries.empty());
      EXPECT_NE(Good.Certificate->InputDigest,
                Combined.Certificate->InputDigest);
      P.Contract.RetainUnauditedNativeBoundaries = false;
    }
    if (!Deferred) {
      EXPECT_EQ(
          Good.Certificate->Relation.NativeAuditBoundaries[0].Boundary.Address,
          Entry + 7);
      P.Image.Segments[0].Data[8] = 0xda; // Dead RCR, still bound to the proof.
      const auto Changed = Check();
      ASSERT_TRUE(Changed.proved()) << Changed.Proof.Diagnostic;
      EXPECT_NE(Good.Certificate->InputDigest,
                Changed.Certificate->InputDigest);
      EXPECT_NE(Good.Certificate->Relation.OriginalDigest,
                Changed.Certificate->Relation.OriginalDigest);
      P.Image.Segments[0].Data[8] = 0xd2;
    }
    LowIRRefinementLimits Short;
    Short.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    ASSERT_TRUE(Check(Short).proved());
    Short.Execution.MaxSolverQueries = Good.Proof.SolverQueries - 1;
    refused(Check(Short), Status::BudgetExceeded);
    Short = {};
    Short.Execution.MaxInstructions = Good.Proof.Instructions - 1;
    refused(Check(Short), Status::BudgetExceeded);
    P.Image.Segments[0].Data[5] = 0x74; // JE makes the bad arm feasible.
    refused(Check(), Status::Unsupported);
    P.Image.Segments[0].Data[5] = 0x75;
    P.Image.Segments[0].Data[15] =
        0xfe; // A wrong rank update is still checked.
    refused(Check(), Status::Different);
  }
}

TEST(BinaryLowIRLoopRefinement, PhysicalCallsAndEarlierStackWrites) {
  // top: jrcxz done; call body; lea rcx,[rcx-1]; jmp top;
  // done: ret; body: add rax,rcx; ret.
  Program P({0xe3, 11, 0xe8, 7, 0, 0, 0, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf3,
             0xc3, 0x48, 0x01, 0xc8, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopCutpoint Cut;
  // The callee entry has one recovery context: the physical call has already
  // written its continuation. The outer header has separate first/loop
  // contexts and cannot serve as a single paired cutpoint.
  Cut.OriginalAddress = Entry + 14;
  unsigned Mappings = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Mappings;
    }
  ASSERT_EQ(Mappings, 1U);
  Cut.UseEntryPrefix = true;
  const auto Parameter = [&](LowIRLoopLocation L) {
    const auto T = NdVar::tmp(Cut.Inputs.size() * 8, L.Bytes);
    Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
    Cut.OriginalState.push_back({L, T});
    Cut.CandidateState.push_back({L, T});
    return T;
  };
  Parameter({LowIRLoopSpace::Register, x86reg::RAX, 8});
  Cut.Rank = {Parameter({LowIRLoopSpace::Register, x86reg::RCX, 8})};
  for (auto Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Parameter({LowIRLoopSpace::Register, Flag, 1});
  LowOp Nonzero;
  Nonzero.Opcode = NdOp::INT_NOTEQUAL;
  Nonzero.Output = NdVar::tmp(64, 1);
  Nonzero.addInput(Cut.Rank.front());
  Nonzero.addInput(NdVar::scalar(0, 8));
  Cut.Expressions = {Nonzero};
  Cut.Predicate = Nonzero.Output;
  LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.RankingChecks, 1U);
  std::set<va_t> Addresses;
  for (const auto &I : Good.Certificate->Instructions)
    EXPECT_TRUE(Addresses.insert(I.Origin.Address).second);
  EXPECT_EQ(Addresses.size(), 7U);
  // The contract includes the return slot written before an earlier cut.
  // A candidate that writes a different continuation cannot hide the effect.
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::STORE) {
        auto Memory = lowMemoryOperands(O);
        ASSERT_TRUE(Memory.Complete);
        O.Inputs[Memory.StoredValue - O.Inputs] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(Check(), Status::Different);
}

TEST(BinaryLowIRLoopInference, JoinedNativeEntryArmsUseCheckedGeneralization) {
  // Both arms initialize eax to 7. The joined loop adds the unsigned count
  // using LEA, keeping the TEST flags unchanged across every iteration.
  // test rdx,rdx; jz alternate; mov eax,7; jmp loop;
  // alternate: mov eax,7; loop: jrcxz done; lea rax,[rax+rcx];
  // lea rcx,[rcx-1]; jmp loop; done: ret.
  Program P({0x48, 0x85, 0xd2, 0x74, 7,    0xb8, 7,    0,    0,    0,
             0xeb, 5,    0xb8, 7,    0,    0,    0,    0xe3, 10,   0x48,
             0x8d, 0x04, 0x08, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf4, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&] {
    return inferAndCheckBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                  Recovery, P.Contract);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
  ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
  bool Generalized = false;
  for (const auto &Cut : Good.Inference.Plan->Cutpoints)
    Generalized |= Cut.GeneralizeEntryPrefix;
  EXPECT_TRUE(Generalized);
  // Changing the other original arm must fail even when the candidate and
  // its inferred plan still describe the old initialization.
  P.Image.Segments[0].Data[13] = 8;
  const auto Bad = Check();
  EXPECT_TRUE(Bad.Inference.inferred()) << Bad.Inference.Diagnostic;
  refused(Bad.Refinement, Status::Different);
}

TEST(BinaryLowIRLoopRefinement, GeneralizedDomainRechecksNativeTrapGuards) {
  // Entry excludes rdx == 0 from the loop. Removing that witness domain
  // admits a trap to the proposed invariant even though every real run is
  // safe. Generalization must reject the proposal, not assume the old guard.
  // test rdx,rdx; jz done; loop: test rdx,rdx; jz fault;
  // jrcxz done; lea rcx,[rcx-1]; jmp loop; done: ret; fault: ud2.
  Program P({0x48, 0x85, 0xd2, 0x74, 13,   0x48, 0x85, 0xd2, 0x74, 9,   0xe3,
             6,    0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf3, 0xc3, 0x0f, 0x0b});
  // Ordinary recovery refuses an opaque trap. Construct the candidate from
  // a separate image with a return on that arm; only the native checker may
  // establish that this difference is unreachable under a proposed domain.
  auto Candidate = P;
  Candidate.Image.Segments[0].Data[19] = 0xc3;
  Candidate.Image.Segments[0].Data[20] = 0x90;
  const auto Recovery = Candidate.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry + 5;
  unsigned Matches = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Matches;
    }
  ASSERT_EQ(Matches, 1U);
  Cut.UseEntryPrefix = true;
  const LowIRLoopLocation Count{LowIRLoopSpace::Register, x86reg::RCX, 8};
  Cut.Inputs = {{LowIRLoopSide::Original, Count, NdVar::tmp(0, 8)}};
  Cut.OriginalState = Cut.CandidateState = {{Count, NdVar::tmp(0, 8)}};
  Cut.Rank = {NdVar::tmp(0, 8)};
  LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  Plan.Cutpoints[0].GeneralizeEntryPrefix = true;
  refused(Check(), Status::ContractViolation);
}

TEST(BinaryLowIRLoopInference, CounterAndCallsUseInferredCheckedPlans) {
  for (bool Calls : {false, true}) {
    Program P(Calls ? std::initializer_list<uint8_t>{0xe3, 11, 0xe8, 7, 0, 0, 0,
                                                     0x48, 0x8d, 0x49, 0xff,
                                                     0xeb, 0xf3, 0xc3, 0x48,
                                                     0x01, 0xc8, 0xc3}
                    : std::initializer_list<uint8_t>{0xe3, 9, 0x48, 0x01, 0xc8,
                                                     0x48, 0x8d, 0x49, 0xff,
                                                     0xeb, 0xf5, 0xc3});
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    const auto R = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
    ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
    EXPECT_EQ(R.Refinement.Certificate->Relation.Scope,
              LowIRRefinementScope::InductiveNativeToLowIRLoops);
    // Origin metadata cannot excuse a different original instruction.
    P.Image.Segments[0].Data[Calls ? 10 : 8] = 0xfe;
    const auto Changed = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    EXPECT_TRUE(Changed.Inference.inferred()) << Changed.Inference.Diagnostic;
    EXPECT_FALSE(Changed.proved());
    EXPECT_FALSE(Changed.Refinement.Certificate);
  }
}

TEST(BinaryLowIRLoopRefinement, NestedPrefixReplaysActualNativeEntry) {
  // xor eax,eax; outer: test rcx,rcx; jz done; mov rdx,r8;
  // inner: test rdx,rdx; jz next; add rax,rdx; dec rdx; jmp inner;
  // next: dec rcx; jmp outer; done: ret.
  Program P({0x31, 0xc0, 0x48, 0x85, 0xc9, 0x74, 0x15, 0x4c, 0x89, 0xc2,
             0x48, 0x85, 0xd2, 0x74, 0x08, 0x48, 0x01, 0xd0, 0x48, 0xff,
             0xca, 0xeb, 0xf3, 0x48, 0xff, 0xc9, 0xeb, 0xe6, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopRefinementPlan Plan;
  for (bool Inner : {false, true}) {
    LowIRLoopCutpoint Cut;
    Cut.OriginalAddress = Entry + (Inner ? 15 : 7);
    unsigned Matches = 0;
    for (const auto &Origin : Recovery.Origins)
      if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
        Cut.CandidateAddress = Origin.ResidualAddress;
        ++Matches;
      }
    ASSERT_EQ(Matches, 1U);
    Cut.UseEntryPrefix = Inner;
    uint64_t Next = 0;
    const auto Temp = [&](uint16_t Bytes) {
      const auto V = NdVar::tmp(Next, Bytes);
      Next += 8;
      return V;
    };
    const auto Parameter = [&](uint64_t Offset, uint16_t Bytes = 8) {
      const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
      const auto T = Temp(Bytes);
      Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
      Cut.OriginalState.push_back({L, T});
      Cut.CandidateState.push_back({L, T});
      return T;
    };
    Parameter(x86reg::RAX);
    const auto OuterCount = Parameter(x86reg::RCX);
    const auto InnerCount = Parameter(x86reg::RDX);
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF})
      Parameter(Flag, 1);
    const auto EntryCount = Temp(8);
    Cut.Inputs.push_back({LowIRLoopSide::Entry,
                          {LowIRLoopSpace::Register, x86reg::RCX, 8},
                          EntryCount});
    const auto Expr = [&](NdOp Code, NdVar A, NdVar B) {
      LowOp Op;
      Op.Opcode = Code;
      Op.Output = Temp(1);
      Op.addInput(A);
      Op.addInput(B);
      Cut.Expressions.push_back(Op);
      return Op.Output;
    };
    Cut.Predicate = Expr(NdOp::INT_LESSEQUAL, OuterCount, EntryCount);
    const auto OuterNonzero =
        Expr(NdOp::INT_NOTEQUAL, OuterCount, NdVar::scalar(0, 8));
    Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, OuterNonzero);
    if (Inner) {
      const auto EntryInner = Temp(8);
      Cut.Inputs.push_back({LowIRLoopSide::Entry,
                            {LowIRLoopSpace::Register, x86reg::R8, 8},
                            EntryInner});
      const auto InnerBound = Expr(NdOp::INT_LESSEQUAL, InnerCount, EntryInner);
      const auto InnerNonzero =
          Expr(NdOp::INT_NOTEQUAL, InnerCount, NdVar::scalar(0, 8));
      Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, InnerBound);
      Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, InnerNonzero);
    }
    Cut.Rank = {OuterCount, NdVar::scalar(Inner ? 0 : 1, 1),
                Inner ? InnerCount : NdVar::scalar(0, 8)};
    Plan.Cutpoints.push_back(std::move(Cut));
  }
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.RankingChecks, 4U);
  EXPECT_GT(Good.Proof.LoopInitiations, 1U);
  // Removing the outer counter's entry bound must not turn a prefix seen
  // only for positive inputs into a globally assumed reachable state.
  Plan.Cutpoints[0].Predicate = NdVar::scalar(1, 1);
  refused(Check(), Status::Different);
  const auto Automatic = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Automatic.Inference.inferred()) << Automatic.Inference.Diagnostic;
  ASSERT_TRUE(Automatic.proved()) << Automatic.Refinement.Proof.Diagnostic;
  EXPECT_GT(Automatic.Inference.Plan->Cutpoints.size(), 1U);
}

TEST(BinaryLowIRLoopInference, PackedFlagsStayConstrainedAcrossWidening) {
  // xor eax,eax; pushfq; pop r11; loop: jrcxz done; push r11; popfq;
  // inc rax; pushfq; pop r11; lea rcx,[rcx-1]; jmp loop; done: ret.
  // The loop carries both a packed flags image and the physical flags bank.
  // Equal bits can have different expressions after abstracting the counter.
  Program P({0x31, 0xc0, 0x9c, 0x41, 0x5b, 0xe3, 0x0f, 0x41,
             0x53, 0x9d, 0x48, 0xff, 0xc0, 0x9c, 0x41, 0x5b,
             0x48, 0x8d, 0x49, 0xff, 0xeb, 0xef, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto R = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
  ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
  EXPECT_GT(R.Inference.WideningRounds, 1U);
  EXPECT_GT(R.Refinement.Proof.RankingChecks, 0U);
}

TEST(BinaryLowIRLoopInference, NativeEqualityExitsUseInductiveCounterBounds) {
  // Two independent unsigned loops, both exiting on equality with an input.
  // xor eax,eax; xor ecx,ecx; outer: cmp rcx,r8; je done; xor edx,edx;
  // inner: cmp rdx,r9; je next; lea rax,[rax+rdx]; inc rdx; jmp inner;
  // next: inc rcx; jmp outer; done: ret.
  Program P({0x31, 0xc0, 0x31, 0xc9, 0x4c, 0x39, 0xc1, 0x74, 0x15, 0x31, 0xd2,
             0x4c, 0x39, 0xca, 0x74, 0x09, 0x48, 0x8d, 0x04, 0x10, 0x48, 0xff,
             0xc2, 0xeb, 0xf2, 0x48, 0xff, 0xc1, 0xeb, 0xe6, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&] {
    return inferAndCheckBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                  Recovery, P.Contract);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
  ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
  EXPECT_GT(Good.Inference.PredicateNodes, 0U);
  EXPECT_GE(Good.Refinement.Proof.RankingChecks, 2U);
  // A recovered plan must not certify the original after changing either
  // increasing counter to a decreasing one.
  for (unsigned Offset : {22, 27}) {
    P.Image.Segments[0].Data[Offset] += 8;
    const auto Bad = Check();
    ASSERT_TRUE(Bad.Inference.inferred()) << Bad.Inference.Diagnostic;
    refused(Bad.Refinement, Status::Different);
    P.Image.Segments[0].Data[Offset] -= 8;
  }
}

TEST(BinaryLowIRLoopInference, OriginHintsAndRecoveryStatusAreUntrusted) {
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  const auto Good = P.recover();
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    auto Recovery = Good;
    if (Kind == 0)
      Recovery.Status = SpecializationStatus::Unsupported;
    if (Kind == 1)
      Recovery.Origins.clear();
    if (Kind == 2)
      Recovery.Origins.insert(Recovery.Origins.end(), Good.Origins.begin(),
                              Good.Origins.end());
    if (Kind == 3)
      for (auto &Origin : Recovery.Origins)
        Origin.NativeInstruction.Address += 0x1000;
    const auto R = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    EXPECT_FALSE(R.proved()) << "kind=" << Kind;
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_FALSE(R.Refinement.Proof.Certificate);
    EXPECT_EQ(R.Inference.inferred(), Kind == 3) << R.Inference.Diagnostic;
  }
}

TEST(BinaryLowIRRefinement, MandatorySystemFlagsRejectCandidateMutation) {
  // pushfq; pop rax; push rax; popfq; ret.
  Program P({0x9c, 0x58, 0x50, 0x9d, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  ASSERT_TRUE(P.check(Recovery.Residual).proved());
  P.Contract.ReturnRegisters.clear();
  P.Contract.ObserveWrittenFrameBytes = false;
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INTRINSIC && O.NumInputs == 2) {
        O.Inputs[1] = NdVar::scalar(2, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, NativeEntryAndImageContractsRemainMandatory) {
  Program P({0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Options.X64CetDisabled = false;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  P.Options.X64CetDisabled = true;
  P.Contract.X64FlagsProfile.reset();
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Contract.X64FlagsProfile = P.Options.X64FlagsProfile;
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RAX, 8), 0});
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.X64FlagsProfile.reset();
  P.Contract.X64FlagsProfile.reset();
  refused(P.check(Recovery.Residual), Status::Unsupported);
}

TEST(BinaryLowIRRefinement, OriginalAndCandidateImmutableReadsHaveEvidence) {
  // mov rax,[rip+0x2ff9]; ret. An independent read-only table begins at 0x4000.
  Program P({0x48, 0x8b, 0x05, 0xf9, 0x2f, 0, 0, 0xc3});
  Segment Table;
  Table.VA = 0x4000;
  Table.Flags = SegmentFlags::Readable;
  Table.Data = {11, 22, 33, 44, 55, 66, 77, 88, 99};
  Table.Size = Table.FileSz = Table.Data.size();
  P.Image.Segments.push_back(Table);
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Folded = P.check(Recovery.Residual);
  ASSERT_TRUE(Folded.proved()) << Folded.Proof.Diagnostic;
  ASSERT_EQ(Folded.Certificate->Reads.size(), 1U);
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Output == NdVar::reg(x86reg::RAX, 8)) {
        O.Opcode = NdOp::LOAD;
        O.NumInputs = 1;
        O.Inputs[0] = NdVar::scalar(0x4000, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  const auto Read = P.check(Recovery.Residual);
  ASSERT_TRUE(Read.proved()) << Read.Proof.Diagnostic;
  ASSERT_EQ(Read.Certificate->Reads.size(), 2U);
  for (const auto &R : Read.Certificate->Reads) {
    EXPECT_EQ(R.Address, 0x4000U);
    EXPECT_EQ(R.Bytes.size(), 8U);
    EXPECT_FALSE(R.Evidence.empty());
  }
  EXPECT_NE(Folded.Certificate->InputDigest, Read.Certificate->InputDigest);
  P.Image.Segments.back().Flags =
      P.Image.Segments.back().Flags | SegmentFlags::Writable;
  refused(P.check(Recovery.Residual), Status::Unsupported);
}

TEST(BinaryLowIRRefinement, TrapsAndUnauditedOriginalArmsAreNotPruned) {
  // xor eax,eax; jz done; rcl eax,1; done: ret. Ordinary recovery prunes RCL,
  // but selected-value refinement still audits the complete direct graph.
  Program P({0x31, 0xc0, 0x74, 2, 0xd1, 0xd0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  Program Trap({0xcc});
  refused(Trap.check(Recovery.Residual), Status::ContractViolation);
}

TEST(BinaryLowIRRefinement, CandidateMustRestoreStackAndOriginalReturnSlot) {
  Program P({0x58, 0x50, 0xc3}); // pop rax; push rax; ret.
  // Ordinary recovery deliberately refuses writes to this slot. Supply an
  // explicit candidate from the independently audited native instructions.
  const auto Native =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  ASSERT_TRUE(Native.proved()) << Native.Proof.Diagnostic;
  LowFunc Candidate;
  Candidate.Entry = Entry;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  for (const auto &I : Native.Certificate->Instructions) {
    auto Boundary = I.Origin;
    Boundary.FirstOp = Block.Ops.size();
    Block.InstructionBoundaries.push_back(Boundary);
    Block.Ops.insert(Block.Ops.end(), I.Ops.begin(), I.Ops.end());
    Block.EndAddr = I.Origin.Address + I.Origin.Size;
  }
  Candidate.Blocks.push_back(std::move(Block));
  const auto Good = P.check(Candidate);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : Candidate.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::STORE) {
        O.Inputs[O.NumInputs - 1] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Candidate), Status::ContractViolation);
}
} // namespace

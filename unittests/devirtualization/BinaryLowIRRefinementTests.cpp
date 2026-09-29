//===- BinaryLowIRRefinementTests.cpp - Original-to-residual relations
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/lift/X86Regs.h"

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
  // xor eax,eax; jz done; rol eax,1; done: ret. Ordinary recovery prunes ROL,
  // but selected-value refinement still audits the complete direct graph.
  Program P({0x31, 0xc0, 0x74, 2, 0xd1, 0xc0, 0xc3});
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

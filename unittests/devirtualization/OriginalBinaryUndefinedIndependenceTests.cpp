//===- OriginalBinaryUndefinedIndependenceTests.cpp - Original graph proof ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRIndependenceStatus;
constexpr va_t Entry = 0x1000;

struct Program {
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceContract Contract;

  explicit Program(std::initializer_list<uint8_t> Bytes) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    Segment Code;
    Code.Name = ".text";
    Code.VA = Entry;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data = Bytes;
    Code.Size = Code.FileSz = Code.Data.size();
    Image.Segments.push_back(std::move(Code));
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    Options.X64CetDisabled = true;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -32, 8};
    Contract.ReturnRegisters = {{x86reg::RAX, 8}};
  }

  BinaryUndefinedIndependenceResult
  check(const LowIRIndependenceLimits &Limits = {}) const {
    return checkBinaryUndefinedIndependence(Image, Entry, Options, Contract,
                                            Limits);
  }

  SpecializationWithIndependenceResult
  recover(const LowIRIndependenceLimits &Limits = {}) const {
    return specializeBinaryInterpreterWithIndependence(Image, Entry, Options,
                                                       Contract, Limits);
  }
};

void expectRefusal(const Program &P, Status Expected,
                   const LowIRIndependenceLimits &Limits = {}) {
  const auto Checked = P.check(Limits);
  EXPECT_EQ(Checked.Proof.Status, Expected) << Checked.Proof.Diagnostic;
  EXPECT_FALSE(Checked.proved());
  EXPECT_FALSE(Checked.Certificate.has_value());
  const auto Gated = P.recover(Limits);
  EXPECT_EQ(Gated.Independence.Proof.Status, Expected)
      << Gated.Independence.Proof.Diagnostic;
  EXPECT_FALSE(Gated.Independence.Certificate.has_value());
  EXPECT_FALSE(Gated.Recovery.complete());
  EXPECT_TRUE(Gated.Recovery.Residual.Blocks.empty());
}

TEST(OriginalBinaryUndefinedIndependence,
     StraightLineCertificateOwnsExactBytesAndFallbackMetadata) {
  // mov eax,7; ret. The optional memory-call route declines both instructions.
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Result = P.recover();
  ASSERT_TRUE(Result.Independence.proved())
      << Result.Independence.Proof.Diagnostic;
  ASSERT_TRUE(Result.Recovery.complete()) << Result.Recovery.Diagnostic;
  ASSERT_FALSE(Result.Recovery.Residual.Blocks.empty());
  const auto &Certificate = *Result.Independence.Certificate;
  ASSERT_EQ(Certificate.Instructions.size(), 2U);
  EXPECT_EQ(Certificate.Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0xb8, 7, 0, 0, 0}));
  EXPECT_EQ(Certificate.Instructions[1].NativeBytes,
            (std::vector<uint8_t>{0xc3}));
  for (const auto &Insn : Certificate.Instructions) {
    EXPECT_EQ(Insn.NativeBytes.size(), Insn.Origin.Size);
    EXPECT_EQ(Insn.UndefinedEffects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_TRUE(Insn.UndefinedEffects.Effects.empty());
    EXPECT_EQ(Insn.UndefinedEffects.OpCount, Insn.Ops.size());
    EXPECT_EQ(Insn.UndefinedEffects.OperationDigest,
              lowUndefinedOperationDigest(Insn.Ops));
  }
  // Mutating the image cannot mutate the certificate's retained byte storage.
  P.Image.Segments[0].Data[1] = 9;
  EXPECT_EQ(Certificate.Instructions[0].NativeBytes[1], 7);
}

TEST(OriginalBinaryUndefinedIndependence,
     UndefinedBranchCannotBeCertifiedAfterOrdinaryRecoveryPrunesIt) {
  // xor eax,eax; lahf; test ah,0x10; jne second; mov eax,0; ret;
  // second: mov eax,0; ret. Both return values agree, but control consumes AF.
  Program P({0x31, 0xc0, 0x9f, 0xf6, 0xc4, 0x10, 0x75, 0x06, 0xb8, 0,
             0,    0,    0,    0xc3, 0xb8, 0,    0,    0,    0,    0xc3});
  // An entry AF constant is killed by XOR's newly arbitrary AF production.
  // The ordinary selected LowIR value must not become architecture evidence.
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::AF, 1), 0});
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::AF, 1), 0});
  const auto Ordinary = specializeBinaryInterpreter(P.Image, Entry, P.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     OrdinaryInputBranchRetainsBothOriginalArms) {
  // test edi,edi; jne second; mov eax,0; ret; second: mov eax,1; ret.
  Program P(
      {0x85, 0xff, 0x75, 0x06, 0xb8, 0, 0, 0, 0, 0xc3, 0xb8, 1, 0, 0, 0, 0xc3});
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  std::vector<va_t> Addresses;
  for (const auto &Insn : Result.Certificate->Instructions)
    Addresses.push_back(Insn.Origin.Address);
  std::sort(Addresses.begin(), Addresses.end());
  EXPECT_EQ(Addresses, (std::vector<va_t>{0x1000, 0x1002, 0x1004, 0x1009,
                                          0x100a, 0x100f}));
}

TEST(OriginalBinaryUndefinedIndependence,
     StaticallyUntakenBranchStillRequiresOriginalBytes) {
  // XOR makes JNE false in ordinary recovery; its target is unmapped 0x100a.
  Program P({0x31, 0xc0, 0x75, 0x06, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Ordinary = specializeBinaryInterpreter(P.Image, Entry, P.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(P, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     StaticallyUntakenBranchStillRequiresArchitectureCoverage) {
  // XOR makes JNE false, but the original arm at 0x100a contains an unaudited
  // SHL. Collecting its bytes alone does not establish complete evidence.
  Program P({0x31, 0xc0, 0x75, 0x06, 0xb8, 7, 0, 0, 0, 0xc3, 0xd1, 0xe0, 0xc3});
  const auto Ordinary = specializeBinaryInterpreter(P.Image, Entry, P.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(P, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     NativeReturnRequiresEntryStackPointerAndReturnAddress) {
  // add rsp,8; mov eax,7; ret: an equal business result does not restore RSP.
  Program Pivot({0x48, 0x83, 0xc4, 8, 0xb8, 7, 0, 0, 0, 0xc3});
  expectRefusal(Pivot, Status::ContractViolation);
  // mov qword ptr [rsp],0; mov eax,7; ret: the return slot is observable even
  // when the caller opts out of ordinary final frame-byte observations.
  Program Slot({0x48, 0xc7, 0x04, 0x24, 0, 0, 0, 0, 0xb8, 7, 0, 0, 0, 0xc3});
  Slot.Contract.ObserveWrittenFrameBytes = false;
  expectRefusal(Slot, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     SavedStackPointerAndReturnSlotMayBeRestored) {
  // push rcx; pop rcx; mov eax,7; ret.
  Program Stack({0x51, 0x59, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto StackResult = Stack.check();
  EXPECT_TRUE(StackResult.proved()) << StackResult.Proof.Diagnostic;
  // mov rdx,[rsp]; mov qword ptr [rsp],0; mov [rsp],rdx; mov eax,7; ret.
  Program Slot({0x48, 0x8b, 0x14, 0x24, 0x48, 0xc7, 0x04, 0x24, 0, 0, 0,
                0,    0x48, 0x89, 0x14, 0x24, 0xb8, 7,    0,    0, 0, 0xc3});
  const auto SlotResult = Slot.check();
  EXPECT_TRUE(SlotResult.proved()) << SlotResult.Proof.Diagnostic;
}

TEST(OriginalBinaryUndefinedIndependence,
     CallsIndirectTransfersCyclesAndOverlappingInstructionsRefuse) {
  Program DirectCall({0xe8, 0, 0, 0, 0, 0xc3});
  expectRefusal(DirectCall, Status::Unsupported);
  // CALL-next can lower to a physical push without a remaining CALL LowOp.
  // POP restores RSP, so native classification must still reject this graph
  // even when its LowIR satisfies the leaf return-preservation obligations.
  Program CallNextPop({0xe8, 0, 0, 0, 0, 0x58, 0xc3});
  expectRefusal(CallNextPop, Status::Unsupported);
  Program IndirectCall({0xff, 0xd0, 0xc3});
  expectRefusal(IndirectCall, Status::Unsupported);
  Program IndirectBranch({0xff, 0xe0});
  expectRefusal(IndirectBranch, Status::Unsupported);
  Program Loop({0xeb, 0xfe});
  expectRefusal(Loop, Status::Unsupported);
  // The branch target 0x1005 decodes as NOP inside the MOV immediate.
  Program Overlap({0x85, 0xff, 0x75, 1, 0xb8, 0x90, 0x90, 0x90, 0x90, 0xc3});
  expectRefusal(Overlap, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     MissingArchitectureEffectsAndProfileProjectionRefuse) {
  Program Shift({0xd1, 0xe0, 0xc3}); // shl eax,1; ret
  expectRefusal(Shift, Status::Unsupported);
  // RDSSPQ is projected to NOP only by the explicit CET-disabled profile.
  // That projection must retain Missing architecture metadata.
  Program ShadowStack({0xf3, 0x48, 0x0f, 0x1e, 0xc8, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Ordinary = specializeBinaryInterpreter(ShadowStack.Image, Entry,
                                                    ShadowStack.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(ShadowStack, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence, ExecutionProfileIsMandatory) {
  for (unsigned Field = 0; Field != 3; ++Field) {
    SCOPED_TRACE(Field);
    Program P({0xb8, 7, 0, 0, 0, 0xc3});
    if (Field == 0)
      P.Options.ExplicitMachineState = false;
    else if (Field == 1)
      P.Options.NormalNonfaultingExecution = false;
    else
      P.Options.X64CetDisabled = false;
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence, EntryContractsMustMatchRecovery) {
  Program MissingFrame({0xb8, 7, 0, 0, 0, 0xc3});
  MissingFrame.Contract.Frame.reset();
  expectRefusal(MissingFrame, Status::Invalid);
  Program Constants({0xb8, 7, 0, 0, 0, 0xc3});
  Constants.Options.EntryConstants.push_back({NdVar::reg(x86reg::RCX, 8), 3});
  expectRefusal(Constants, Status::Invalid);
  Program Endian({0xb8, 7, 0, 0, 0, 0xc3});
  Endian.Contract.ByteOrder = llvm::endianness::big;
  expectRefusal(Endian, Status::Invalid);
}

TEST(OriginalBinaryUndefinedIndependence,
     EntryFrameCannotOverlapImmutableCode) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), Entry});
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), Entry});
  // The bound [-32,8) frame overlaps the RX instruction mapping. A proof
  // cannot succeed using an inconsistent mutable-frame environment.
  expectRefusal(P, Status::InfeasibleEntry);
}

TEST(OriginalBinaryUndefinedIndependence, CollectionBudgetsDoNotPublishPrefix) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  LowIRIndependenceLimits Instructions;
  Instructions.MaxInstructions = 1;
  expectRefusal(P, Status::BudgetExceeded, Instructions);
  LowIRIndependenceLimits Blocks;
  Blocks.MaxBlockVisits = 1;
  expectRefusal(P, Status::BudgetExceeded, Blocks);
  LowIRIndependenceLimits Operations;
  Operations.MaxOperations = 1;
  expectRefusal(P, Status::BudgetExceeded, Operations);
}

TEST(OriginalBinaryUndefinedIndependence,
     WritableConflictingAndRelocatedMappingsRefuse) {
  Program Writable({0xb8, 7, 0, 0, 0, 0xc3});
  Writable.Image.Segments[0].Flags =
      Writable.Image.Segments[0].Flags | SegmentFlags::Writable;
  expectRefusal(Writable, Status::Unsupported);
  Program Conflicting({0xb8, 7, 0, 0, 0, 0xc3});
  Segment Duplicate = Conflicting.Image.Segments[0];
  Conflicting.Image.Segments.push_back(std::move(Duplicate));
  expectRefusal(Conflicting, Status::Unsupported);
  Program Fixed({0xb8, 7, 0, 0, 0, 0xc3});
  Fixed.Image.Relocations.push_back(
      {.Address = Entry + 1, .Type = llvm::ELF::R_X86_64_64});
  expectRefusal(Fixed, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     BaseRelocationScanningHasAMetadataBudget) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  LowIRIndependenceLimits Limits;
  Limits.MaxInstructions = 16;
  const auto Baseline = P.check(Limits);
  ASSERT_TRUE(Baseline.proved()) << Baseline.Proof.Diagnostic;
  // These fixups do not overlap code. Their quantity alone exceeds the
  // allowed input metadata and must be rejected before repeated scans.
  for (unsigned I = 0; I != 17; ++I)
    P.Image.BaseRelocations.push_back({0x4000 + 8 * I, 10});
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     EqualLengthByteChangesBindEvenWhenLowIRIsIdentical) {
  // Distinct ignored segment prefixes on NOP have the same lifted behavior
  // and instruction boundaries; binary evidence must still bind their bytes.
  Program P({0x2e, 0x90, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Before = P.check();
  ASSERT_TRUE(Before.proved()) << Before.Proof.Diagnostic;
  P.Image.Segments[0].Data[0] = 0x3e;
  const auto After = P.check();
  ASSERT_TRUE(After.proved()) << After.Proof.Diagnostic;
  EXPECT_EQ(Before.Certificate->LowIR.InputDigest,
            After.Certificate->LowIR.InputDigest);
  EXPECT_NE(Before.Certificate->InputDigest, After.Certificate->InputDigest);
  ASSERT_FALSE(Before.Certificate->Instructions.empty());
  ASSERT_FALSE(After.Certificate->Instructions.empty());
  EXPECT_EQ(Before.Certificate->Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0x2e, 0x90}));
  EXPECT_EQ(After.Certificate->Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0x3e, 0x90}));
}

} // namespace

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
     PhysicalCallAndReturnPreserveMachineState) {
  // mov eax,7; call helper; ret; helper: add eax,1; ret.
  Program P({0xb8, 7, 0, 0, 0, 0xe8, 1, 0, 0, 0, 0xc3, 0x83, 0xc0, 1, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
  EXPECT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
  EXPECT_EQ(R.Independence.Certificate->LowIR.Scope,
            LowIRIndependenceScope::CompleteFiniteNativePaths);
  const auto &Instructions = R.Independence.Certificate->Instructions;
  EXPECT_EQ(std::count_if(Instructions.begin(), Instructions.end(),
                          [](const auto &I) { return I.IsNativeCall; }),
            1);
  EXPECT_EQ(R.Independence.Proof.Paths, 1u);
}

TEST(OriginalBinaryUndefinedIndependence,
     RepeatedHelperUsesActualContinuationSlots) {
  // Two calls to the same helper must not share their different return slots.
  Program P({0xb8, 7, 0, 0, 0, 0xe8, 6,    0,    0, 0,
             0xe8, 1, 0, 0, 0, 0xc3, 0x83, 0xc0, 1, 0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  const auto &Trace = R.Certificate->LowIR.Instructions;
  EXPECT_EQ(
      std::count_if(Trace.begin(), Trace.end(),
                    [](const auto &I) { return I.Boundary.Address == 0x1013; }),
      2);
}

TEST(OriginalBinaryUndefinedIndependence,
     MemoryCallReadsBeforePushOverwritesTarget) {
  // mov rax,helper; mov [rsp-8],rax; call [rsp-8]; ret; helper: mov eax,7; ret.
  Program P({0x48, 0xb8, 0x14, 0x10, 0,    0,    0,    0,    0,
             0,    0x48, 0x89, 0x44, 0x24, 0xf8, 0xff, 0x54, 0x24,
             0xf8, 0xc3, 0xb8, 7,    0,    0,    0,    0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 1u);
}

TEST(OriginalBinaryUndefinedIndependence,
     PartialReturnSlotWriteSelectsRealTarget) {
  // call helper; ret; helper: mov word [rsp],0x1010; ret; padding; destination.
  // The original fallthrough is 0x1005, not the modified return target 0x1010.
  Program P({0xe8, 1,    0,    0,    0,    0xc3, 0x66, 0xc7, 0x04, 0x24, 0x10,
             0x10, 0xc3, 0x90, 0x90, 0x90, 0xb8, 9,    0,    0,    0,    0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  const auto &Trace = R.Certificate->LowIR.Instructions;
  EXPECT_TRUE(std::any_of(Trace.begin(), Trace.end(), [](const auto &I) {
    return I.Boundary.Address == 0x1010;
  }));
  EXPECT_FALSE(std::any_of(Trace.begin(), Trace.end(), [](const auto &I) {
    return I.Boundary.Address == 0x1005;
  }));
}

TEST(OriginalBinaryUndefinedIndependence,
     DiscardedCallFrameReachesOuterReturn) {
  // call helper; ret; helper: add rsp,8; ret.
  Program P({0xe8, 1, 0, 0, 0, 0xc3, 0x48, 0x83, 0xc4, 8, 0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 1u);
}

TEST(OriginalBinaryUndefinedIndependence, CalleeCannotCorruptEntryReturnSlot) {
  // The internal return slot is [rsp]; [rsp+8] is the outer entry slot.
  Program P(
      {0xe8, 1, 0, 0, 0, 0xc3, 0x48, 0xc7, 0x44, 0x24, 8, 0, 0, 0, 0, 0xc3});
  expectRefusal(P, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteIndirectTargetsRequireFullCoverage) {
  // and edi,1; add edi,0x1010; jmp rdi; padding; ret; ret.
  Program P({0x83, 0xe7, 1, 0x81, 0xc7, 0x10, 0x10, 0, 0, 0xff, 0xe7, 0x90,
             0x90, 0x90, 0x90, 0x90, 0xc3, 0xc3});
  LowIRIndependenceLimits Limits;
  Limits.MaxIndirectTargets = 2;
  const auto R = P.check(Limits);
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 2u);
  Limits.MaxIndirectTargets = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits.MaxIndirectTargets = 2;
  Limits.MaxPaths = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     UndefinedIndirectTargetIsCheckedBeforePartition) {
  // XOR's arbitrary AF is copied into AH, isolated, then used as a target bit.
  Program P({0x31, 0xc0, 0x9f, 0x25, 0, 0x10, 0, 0, 0x48, 0x0d, 0, 0x20, 0, 0,
             0xff, 0xe0});
  const auto R = P.check();
  EXPECT_EQ(R.Proof.Status, Status::Dependent) << R.Proof.Diagnostic;
  EXPECT_NE(R.Proof.Diagnostic.find("control target"), std::string::npos);
  EXPECT_FALSE(R.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     UndefinedChoicesSurvivePhysicalCallAndReturn) {
  // xor eax,eax; call helper; lahf; isolate AF; form target; jmp rax;
  // helper: ret. The caller's arbitrary AF crosses both CALL and RET.
  Program P({0x31, 0xc0, 0xe8, 0x0e, 0, 0,    0, 0x9f, 0x25, 0,    0x10,
             0,    0,    0x48, 0x0d, 0, 0x20, 0, 0,    0xff, 0xe0, 0xc3});
  const auto R = P.check();
  EXPECT_EQ(R.Proof.Status, Status::Dependent) << R.Proof.Diagnostic;
  EXPECT_NE(R.Proof.Diagnostic.find("control target"), std::string::npos);
  EXPECT_FALSE(R.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     ImmutableReadsBindBytesAndDataMapping) {
  // mov rax,[rip+1]; ret; immutable eight-byte scalar.
  for (bool FixedStack : {false, true}) {
    SCOPED_TRACE(FixedStack);
    Program P({0x48, 0x8b, 5, 1, 0, 0, 0, 0xc3, 7, 0, 0, 0, 0, 0, 0, 0});
    if (FixedStack) {
      P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), 0x8000});
      P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), 0x8000});
    }
    const auto First = P.check();
    ASSERT_TRUE(First.proved()) << First.Proof.Diagnostic;
    ASSERT_EQ(First.Certificate->Reads.size(), 1u);
    EXPECT_EQ(First.Certificate->Reads[0].Bytes[0], 7u);
    P.Image.Segments[0].Data[8] = 9;
    const auto Second = P.check();
    ASSERT_TRUE(Second.proved()) << Second.Proof.Diagnostic;
    EXPECT_NE(First.Certificate->InputDigest, Second.Certificate->InputDigest);
    P.Image.Segments[0].FileOff += 32;
    const auto Third = P.check();
    ASSERT_TRUE(Third.proved()) << Third.Proof.Diagnostic;
    EXPECT_NE(Second.Certificate->InputDigest, Third.Certificate->InputDigest);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     SeparateImmutableDataMappingIsBoundWithoutNativeInstructions) {
  // mov rax,[rip+0xff9]; ret. The data owner contains no fetched instructions,
  // so a code-mapping digest cannot accidentally cover its mapping metadata.
  Program P({0x48, 0x8b, 5, 0xf9, 0x0f, 0, 0, 0xc3});
  Segment Data;
  Data.VA = 0x2000;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {7, 0, 0, 0, 0, 0, 0, 0};
  Data.Size = Data.FileSz = Data.Data.size();
  P.Image.Segments.push_back(Data);
  const auto First = P.check();
  ASSERT_TRUE(First.proved()) << First.Proof.Diagnostic;
  ASSERT_EQ(First.Certificate->Reads.size(), 1U);
  EXPECT_EQ(First.Certificate->Reads[0].Address, Data.VA);
  for (const auto &Insn : First.Certificate->Instructions)
    EXPECT_LT(Insn.Origin.Address, Data.VA);
  P.Image.Segments[1].FileOff += 32;
  const auto Second = P.check();
  ASSERT_TRUE(Second.proved()) << Second.Proof.Diagnostic;
  EXPECT_EQ(First.Certificate->Reads[0].Bytes,
            Second.Certificate->Reads[0].Bytes);
  EXPECT_NE(First.Certificate->InputDigest, Second.Certificate->InputDigest);
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
  // add rsp,8; mov eax,7; ret: this is now a physical internal transfer, not
  // an outer exit. Its target load exceeds the accessible frame contract.
  Program Pivot({0x48, 0x83, 0xc4, 8, 0xb8, 7, 0, 0, 0, 0xc3});
  expectRefusal(Pivot, Status::Unsupported);
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
     CallNextPreservesOnePushWhileUnboundedTargetsAndCyclesRefuse) {
  Program DirectCall({0xe8, 0, 0, 0, 0, 0xc3});
  const auto Direct = DirectCall.check();
  ASSERT_TRUE(Direct.proved()) << Direct.Proof.Diagnostic;
  // CALL-next can lower to a physical push without a remaining CALL LowOp.
  // POP restores RSP, so native classification must still reject this graph
  // even when its LowIR satisfies the leaf return-preservation obligations.
  Program CallNextPop({0xe8, 0, 0, 0, 0, 0x58, 0xc3});
  const auto Pop = CallNextPop.check();
  ASSERT_TRUE(Pop.proved()) << Pop.Proof.Diagnostic;
  Program IndirectCall({0xff, 0xd0, 0xc3});
  expectRefusal(IndirectCall, Status::BudgetExceeded);
  Program IndirectBranch({0xff, 0xe0});
  expectRefusal(IndirectBranch, Status::BudgetExceeded);
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

TEST(OriginalBinaryUndefinedIndependence,
     LinkedFormatsRequireCompleteImageAndExceptionCoverage) {
  for (auto Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    Program P({0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret
    P.Image.Format = Format;
    const auto Baseline = P.recover();
    ASSERT_TRUE(Baseline.Independence.proved())
        << Baseline.Independence.Proof.Diagnostic;
    ASSERT_TRUE(Baseline.Recovery.complete()) << Baseline.Recovery.Diagnostic;

    // Partial coverage has no structural or localized-function evidence in
    // this fixture; it must not acquire the meaning of an empty complete map.
    for (auto Parse :
         {ExceptionParseStatus::Partial, ExceptionParseStatus::Malformed}) {
      SCOPED_TRACE(static_cast<unsigned>(Parse));
      P.Image.ExceptionMetadata.ParseStatus = Parse;
      expectRefusal(P, Status::Unsupported);
    }
    P.Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    P.Image.IsRelocatable = true;
    expectRefusal(P, Status::Invalid);
    P.Image.IsRelocatable = false;

    if (Format == BinaryFormat::COFF) {
      P.Image.LoadOnlyFunctionEntries.insert(Entry);
      expectRefusal(P, Status::Unsupported);
    }
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

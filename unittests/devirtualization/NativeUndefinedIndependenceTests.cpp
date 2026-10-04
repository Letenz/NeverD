//===- NativeUndefinedIndependenceTests.cpp - Native proof boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/NativeUndefinedIndependence.h"
#include "gtest/gtest.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/Errc.h"

#include <map>

using namespace neverd;
using namespace neverd::analysis;

namespace {

/// Synthetic provider records exercise the native driver's metadata and
/// execution contracts independently of the decoder. The malformed-width
/// case deliberately supplies an operation no valid x64 decoder would emit.
class NativeProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Instructions;
  std::map<va_t, unsigned> Fetches;
  std::map<va_t, uint8_t> ReadBytes;

  void prependLoad(va_t Address, va_t ReadAddress) {
    auto &Insn = Instructions.at(Address);
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Addr = Address;
    Load.Output = NdVar::reg(0, 1);
    Load.addInput(NdVar::scalar(ReadAddress, 8));
    Insn.Ops.insert(Insn.Ops.begin(), Load);
    for (size_t I = 0; I != Insn.Ops.size(); ++I)
      Insn.Ops[I].Seq = I;
    Insn.Origin.OpCount = Insn.Ops.size();
    certify(Insn);
  }

  void conditional(va_t Address, va_t Target, bool Taken) {
    indirect(Address, Target);
    auto &Insn = Instructions.at(Address);
    Insn.Ops[0].Opcode = NdOp::COND_BR;
    Insn.Ops[0].addInput(NdVar::scalar(Taken, 1));
    Insn.Origin.ControlFlags = LowInstructionControlFlag::Branch |
                               LowInstructionControlFlag::Conditional;
    Insn.Origin.Immediate = Target;
    certify(Insn);
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    if (Bytes != 1 || !ReadBytes.count(Address))
      return std::nullopt;
    return SpecializationImmutableRead{{ReadBytes.at(Address)},
                                       "synthetic immutable byte"};
  }

  void indirect(va_t Address, va_t Target, uint16_t TargetBytes = 8) {
    auto &Insn = Instructions[Address];
    Insn = {};
    LowOp Branch;
    Branch.Opcode = NdOp::INDIR_BR;
    Branch.Addr = Address;
    Branch.addInput(NdVar::scalar(Target, TargetBytes));
    Insn.Ops = {Branch};
    Insn.Origin.Address = Address;
    Insn.Origin.Size = 2;
    Insn.Origin.OpCount = 1;
    Insn.Origin.Control = LowInstructionControl::Branch;
    Insn.Origin.ControlFlags =
        LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect;
    Insn.Fallthrough.Address = Address + 2;
    Insn.NativeBytes = {0xff, 0xe0};
    certify(Insn);
  }

  void ret(va_t Address) {
    auto &Insn = Instructions[Address];
    Insn = {};
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Address;
    Insn.Ops = {Return};
    Insn.Origin.Address = Address;
    Insn.Origin.Size = 1;
    Insn.Origin.OpCount = 1;
    Insn.Origin.Control = LowInstructionControl::Return;
    Insn.Origin.ControlFlags = LowInstructionControlFlag::Return;
    Insn.Fallthrough.Address = Address + 1;
    Insn.NativeStackControl = SpecializationNativeStackControl::Return;
    Insn.NativeBytes = {0xc3};
    certify(Insn);
  }

  void untakenTrap() {
    indirect(0x100, 0x200);
    auto &Branch = Instructions.at(0x100);
    Branch.Ops[0].Opcode = NdOp::COND_BR;
    Branch.Ops[0].addInput(NdVar::scalar(0, 1));
    Branch.Origin.ControlFlags = LowInstructionControlFlag::Branch |
                                 LowInstructionControlFlag::Conditional;
    Branch.Origin.Immediate = 0x200;
    certify(Branch);
    ret(0x102);
    auto &Trap = Instructions[0x200];
    Trap = {};
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Addr = 0x200;
    Op.addInput(NdVar::scalar(static_cast<uint64_t>(Intrinsic::Int3), 2));
    Trap.Ops = {Op};
    Trap.Origin.Address = 0x200;
    Trap.Origin.Size = 1;
    Trap.Origin.OpCount = 1;
    Trap.Origin.Control = LowInstructionControl::Terminator;
    Trap.Origin.ControlFlags = LowInstructionControlFlag::Terminator |
                               LowInstructionControlFlag::Resumable;
    Trap.Fallthrough.Address = 0x201;
    Trap.NativeBytes = {0xcc};
    certify(Trap);
    Trap.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
  }

  void untakenUnaudited() {
    untakenTrap();
    auto &Insn = Instructions.at(0x200);
    LowOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Addr = 0x200;
    Op.Output = NdVar::reg(0, 8);
    Op.addInput(NdVar::reg(8, 8));
    Insn.Ops = {Op};
    Insn.Origin.Size = 2;
    Insn.Origin.Control = LowInstructionControl::None;
    Insn.Origin.ControlFlags = LowInstructionControlFlag::None;
    Insn.Fallthrough.Address = 0x202;
    Insn.NativeBytes = {0xd1, 0xd0};
    certify(Insn);
    Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
  }

  void flags(bool Pop) {
    auto &Insn = Instructions[0x100];
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Addr = 0x100;
    Op.addInput(NdVar::scalar(
        static_cast<uint64_t>(Pop ? Intrinsic::Popf : Intrinsic::Pushf), 2));
    if (Pop)
      Op.addInput(NdVar::scalar(2, 8));
    else
      Op.Output = NdVar::tmp(0, 8);
    Insn.Ops = {Op};
    Insn.Origin.Address = 0x100;
    Insn.Origin.Size = 1;
    Insn.Origin.OpCount = 1;
    Insn.Fallthrough.Address = 0x101;
    Insn.NativeBytes = {static_cast<uint8_t>(Pop ? 0x9d : 0x9c)};
    certify(Insn);
    ret(0x101);
  }

  void rdssp() {
    flags(false);
    auto &Insn = Instructions.at(0x100);
    Insn.Ops[0].Opcode = NdOp::NOP;
    Insn.Ops[0].Output = {};
    Insn.Ops[0].NumInputs = 0;
    Insn.Origin.Size = 5;
    Insn.Fallthrough.Address = 0x105;
    Insn.NativeBytes = {0xf3, 0x48, 0x0f, 0x1e, 0xc8};
    Insn.ProfileProjection =
        InterpreterProfileProjection::CetDisabledReadShadowStackV1;
    certify(Insn);
    Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
    Instructions.erase(0x101);
    ret(0x105);
  }

  void untakenProfileTrap() {
    untakenTrap();
    auto &Insn = Instructions.at(0x200);
    Insn.Ops[0].Inputs[0] =
        NdVar::scalar(static_cast<uint64_t>(Intrinsic::CetIncSsp), 2);
    Insn.Ops[0].Output = NdVar::reg(0, 8);
    Insn.Origin.Size = 5;
    Insn.Origin.ControlFlags = LowInstructionControlFlag::Terminator;
    Insn.Fallthrough.Address = 0x205;
    Insn.NativeBytes = {0xf3, 0x48, 0x0f, 0xae, 0xe8};
    Insn.ProfileProjection =
        InterpreterProfileProjection::CetDisabledIncrementShadowStackTrapV1;
    certify(Insn);
    Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    ++Fetches[Cursor.Address];
    const auto Found = Instructions.find(Cursor.Address);
    if (Found == Instructions.end() || Cursor.Mode != InstructionMode::Default)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing synthetic native instruction");
    return Found->second;
  }

private:
  static void certify(SpecializationInstruction &Insn) {
    Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Complete;
    Insn.UndefinedEffects.OpCount = Insn.Ops.size();
    Insn.UndefinedEffects.OperationDigest =
        lowUndefinedOperationDigest(Insn.Ops);
  }
};

LowIRIndependenceContract contract() {
  LowIRIndependenceContract Result;
  Result.EntryConstants.push_back({NdVar::reg(32, 8), 0x10000});
  Result.Frame = LowIRIndependenceFrame{{32, 8}, 0, 8, {}};
  Result.PreservedRegisters.push_back({32, 8});
  Result.PreservedFrameRanges.push_back({0, 8});
  return Result;
}

TEST(NativeUndefinedIndependence, OverlappingIndirectEntriesChargeEveryByte) {
  NativeProvider Provider;
  Provider.indirect(0x100, 0x101);
  Provider.Instructions.at(0x100).NativeBytes[1] = 0xc3;
  Provider.ret(0x101);
  auto C = contract();
  LowIRIndependenceLimits Limits;
  const auto Check = [&] {
    return detail::checkNativeUndefinedIndependence(Provider, {0x100}, C,
                                                    Limits);
  };
  const auto Strict = Check();
  EXPECT_EQ(Strict.Proof.Status, LowIRIndependenceStatus::Unsupported);
  EXPECT_FALSE(Strict.Proof.Certificate);
  EXPECT_TRUE(Strict.Instructions.empty());
  C.AllowOverlappingNativeInstructions = true;
  const auto Good = Check();
  ASSERT_TRUE(Good.Proof.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Instructions.size(), 2u);
  Limits.MaxNativeInstructionBytes = 3;
  const auto Exact = Check();
  ASSERT_TRUE(Exact.Proof.proved()) << Exact.Proof.Diagnostic;
  EXPECT_NE(Good.Proof.Certificate->InputDigest,
            Exact.Proof.Certificate->InputDigest);
  for (unsigned Budget : {0, 1, 2}) {
    Limits.MaxNativeInstructionBytes = Budget;
    const auto Limited = Check();
    EXPECT_EQ(Limited.Proof.Status, LowIRIndependenceStatus::BudgetExceeded)
        << Limited.Proof.Diagnostic;
    EXPECT_FALSE(Limited.Proof.Certificate);
    EXPECT_TRUE(Limited.Instructions.empty());
  }
  Limits.MaxNativeInstructionBytes = 3;
  Provider.Instructions.at(0x101).NativeBytes[0] ^= 1;
  const auto Conflict = Check();
  EXPECT_EQ(Conflict.Proof.Status, LowIRIndependenceStatus::Invalid)
      << Conflict.Proof.Diagnostic;
  EXPECT_FALSE(Conflict.Proof.Certificate);
  EXPECT_TRUE(Conflict.Instructions.empty());
}

TEST(NativeUndefinedIndependence,
     ContainedInstructionBytesAgreeInBothCollectionOrders) {
  for (bool InnerFirst : {false, true}) {
    SCOPED_TRACE(InnerFirst);
    NativeProvider Provider;
    Provider.indirect(0x100, InnerFirst ? 0x201 : 0x200);
    Provider.indirect(0x200, 0x300);
    Provider.indirect(0x201, InnerFirst ? 0x200 : 0x300);
    Provider.ret(0x300);
    auto &Outer = Provider.Instructions.at(0x200);
    Outer.Origin.Size = 4;
    Outer.Fallthrough.Address = 0x204;
    Outer.NativeBytes = {0x90, 0xff, 0xe0, 0x90};
    // In this order a direct edge collects the contained entry even though
    // the execution follows the other arm. Its bytes still must agree.
    if (!InnerFirst) {
      Provider.conditional(0x300, 0x201, false);
      Provider.ret(0x302);
    }
    auto C = contract();
    C.AllowOverlappingNativeInstructions = true;
    const auto Check = [&] {
      return detail::checkNativeUndefinedIndependence(Provider, {0x100}, C, {});
    };
    const auto Good = Check();
    ASSERT_TRUE(Good.Proof.proved()) << Good.Proof.Diagnostic;
    ASSERT_EQ(Provider.Fetches.at(0x201), 1u);
    Provider.Instructions.at(0x201).NativeBytes[1] ^= 1;
    const auto Conflict = Check();
    EXPECT_EQ(Conflict.Proof.Status, LowIRIndependenceStatus::Invalid)
        << Conflict.Proof.Diagnostic;
    EXPECT_FALSE(Conflict.Proof.Certificate);
    EXPECT_TRUE(Conflict.Instructions.empty());
  }
}

TEST(NativeUndefinedIndependence,
     CodeAndImmutableReadsShareEvidenceInBothOrders) {
  for (bool ReadFirst : {false, true}) {
    SCOPED_TRACE(ReadFirst);
    NativeProvider Provider;
    Provider.indirect(0x100, 0x200);
    Provider.indirect(0x200, 0x300);
    Provider.ret(0x300);
    const va_t ReadAddress = ReadFirst ? 0x200 : 0x100;
    Provider.prependLoad(ReadFirst ? 0x100 : 0x200, ReadAddress);
    Provider.ReadBytes[ReadAddress] = 0xff;
    auto C = contract();
    C.AllowOverlappingNativeInstructions = true;
    const auto Check = [&] {
      return detail::checkNativeUndefinedIndependence(Provider, {0x100}, C, {});
    };
    const auto Good = Check();
    ASSERT_TRUE(Good.Proof.proved()) << Good.Proof.Diagnostic;
    ASSERT_EQ(Good.Reads.size(), 1u);
    Provider.ReadBytes[ReadAddress] ^= 1;
    const auto Conflict = Check();
    EXPECT_EQ(Conflict.Proof.Status, LowIRIndependenceStatus::Invalid)
        << Conflict.Proof.Diagnostic;
    EXPECT_FALSE(Conflict.Proof.Certificate);
    EXPECT_TRUE(Conflict.Instructions.empty());
  }
}

TEST(NativeUndefinedIndependence,
     CandidateReadChecksNativeBytesButLabelsAreNotByteEvidence) {
  NativeProvider Provider;
  Provider.ret(0x100);
  Provider.ret(0x200);
  Provider.prependLoad(0x200, 0x100);
  LowFunc Candidate;
  Candidate.Entry = 0x100;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = 0x100;
  B.EndAddr = 0x101;
  B.Ops = Provider.Instructions.at(0x200).Ops;
  // The label matches the native RET address, but this is a different LowIR
  // body. It is checked by execution, not reinterpreted as native bytes.
  for (auto &Op : B.Ops)
    Op.Addr = 0x100;
  B.InstructionBoundaries = {Provider.Instructions.at(0x200).Origin};
  B.InstructionBoundaries.front().Address = 0x100;
  Candidate.Blocks.push_back(std::move(B));
  Provider.Instructions.erase(0x200);
  Provider.ReadBytes[0x100] = 0xc3;
  auto C = contract();
  C.AllowOverlappingNativeInstructions = true;
  const auto Check = [&] {
    return detail::checkNativeLowIRRefinement(
        Provider, {0x100}, Candidate, C, LowIRRefinementWitness::LiftedBits,
        {});
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.Proof.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Instructions.size(), 1u);
  ASSERT_EQ(Good.Reads.size(), 1u);
  Provider.ReadBytes[0x100] ^= 1;
  const auto Conflict = Check();
  EXPECT_EQ(Conflict.Proof.Status, LowIRRefinementStatus::Invalid)
      << Conflict.Proof.Diagnostic;
  EXPECT_FALSE(Conflict.Proof.Certificate);
  EXPECT_TRUE(Conflict.Instructions.empty());
}

TEST(NativeUndefinedIndependence,
     UnauditedReceiptsBindInnerDigestAndStopFetching) {
  NativeProvider Provider;
  Provider.untakenUnaudited();
  auto C = contract();
  C.RetainUnauditedNativeBoundaries = true;
  const auto Check = [&] {
    return detail::checkNativeUndefinedIndependence(Provider, {0x100}, C, {});
  };
  const auto First = Check();
  ASSERT_TRUE(First.Proof.proved()) << First.Proof.Diagnostic;
  EXPECT_FALSE(Provider.Fetches.count(0x202));
  ASSERT_EQ(First.Proof.Certificate->NativeAuditBoundaries.size(), 1u);
  auto &Boundary = Provider.Instructions.at(0x200);
  Boundary.NativeBytes.back() ^= 8;
  const auto ChangedBytes = Check();
  ASSERT_TRUE(ChangedBytes.Proof.proved()) << ChangedBytes.Proof.Diagnostic;
  EXPECT_NE(First.Proof.Certificate->InputDigest,
            ChangedBytes.Proof.Certificate->InputDigest);
  Boundary.Ops[0].Inputs[0] = NdVar::reg(16, 8);
  Boundary.UndefinedEffects.OperationDigest =
      lowUndefinedOperationDigest(Boundary.Ops);
  const auto ChangedOps = Check();
  ASSERT_TRUE(ChangedOps.Proof.proved()) << ChangedOps.Proof.Diagnostic;
  EXPECT_NE(ChangedBytes.Proof.Certificate->InputDigest,
            ChangedOps.Proof.Certificate->InputDigest);
  C.RetainUnauditedNativeBoundaries = false;
  const auto Strict = Check();
  EXPECT_EQ(Strict.Proof.Status, LowIRIndependenceStatus::Unsupported);
  EXPECT_FALSE(Strict.Proof.Certificate);
  EXPECT_TRUE(Strict.Instructions.empty());
}

TEST(NativeUndefinedIndependence,
     UnauditedBoundaryRejectsMalformedOrPartialEvidence) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    NativeProvider Provider;
    Provider.untakenUnaudited();
    auto &I = Provider.Instructions.at(0x200);
    switch (Mutation) {
    case 0:
      I.UndefinedEffects.OperationDigest.clear();
      break;
    case 1:
      I.UndefinedEffects.OperationDigest = "stale";
      break;
    case 2:
      I.Ops[0].Inputs[0] = NdVar::reg(16, 8);
      break;
    case 3:
      I.UndefinedEffects.Effects.push_back({1, NdVar::reg(0, 8), 0, 1, {}});
      break;
    case 4:
      I.UndefinedEffects.Coverage = LowUndefinedCoverage::Unsupported;
      break;
    case 5:
      I.UndefinedEffects.Coverage = static_cast<LowUndefinedCoverage>(99);
      break;
    case 6:
      ++I.UndefinedEffects.OpCount;
      break;
    case 7:
      I.NativeBytes.clear();
      break;
    case 8:
      ++I.Origin.OpCount;
      break;
    case 9:
      I.Ops[0].Opcode = NdOp::CALL;
      I.Ops[0].Output = {};
      I.Ops[0].Inputs[0] = NdVar::scalar(0x300, 8);
      I.Origin.Control = LowInstructionControl::Call;
      I.Origin.ControlFlags = LowInstructionControlFlag::Call;
      I.Origin.Immediate = 0x300;
      I.UndefinedEffects.OperationDigest = lowUndefinedOperationDigest(I.Ops);
      break; // Missing physical CALL evidence must still be checked.
    }
    auto C = contract();
    C.RetainUnauditedNativeBoundaries = true;
    const auto R =
        detail::checkNativeUndefinedIndependence(Provider, {0x100}, C, {});
    EXPECT_FALSE(R.Proof.proved());
    EXPECT_FALSE(R.Proof.Certificate);
    EXPECT_TRUE(R.Instructions.empty());
    EXPECT_EQ(R.Proof.Status, Mutation == 4 || Mutation == 5 || Mutation == 9
                                  ? LowIRIndependenceStatus::Unsupported
                                  : LowIRIndependenceStatus::Invalid)
        << R.Proof.Diagnostic;
  }
}

TEST(NativeUndefinedIndependence, NativeBoundaryAddressDoesNotSkipCandidate) {
  NativeProvider Provider;
  Provider.untakenUnaudited();
  auto C = contract();
  C.ReturnRegisters = {{0, 8}};
  C.RetainUnauditedNativeBoundaries = true;
  LowFunc Candidate;
  Candidate.Entry = 0x200;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = 0x200;
  B.EndAddr = 0x203;
  B.Ops = Provider.Instructions.at(0x200).Ops;
  B.Ops.front().Inputs[0] = NdVar::reg(0, 8);
  B.InstructionBoundaries = {Provider.Instructions.at(0x200).Origin};
  auto Return = Provider.Instructions.at(0x102).Ops.front();
  Return.Addr = 0x202;
  auto ReturnBoundary = Provider.Instructions.at(0x102).Origin;
  ReturnBoundary.Address = 0x202;
  ReturnBoundary.FirstOp = 1;
  B.Ops.push_back(Return);
  B.InstructionBoundaries.push_back(ReturnBoundary);
  Candidate.Blocks.push_back(std::move(B));
  const auto Check = [&] {
    return detail::checkNativeLowIRRefinement(
        Provider, {0x100}, Candidate, C, LowIRRefinementWitness::LiftedBits,
        {});
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.Proof.proved()) << Good.Proof.Diagnostic;
  Candidate.Blocks.front().Ops.front().Inputs[0] = NdVar::scalar(7, 8);
  const auto Bad = Check();
  EXPECT_EQ(Bad.Proof.Status, LowIRRefinementStatus::Different)
      << Bad.Proof.Diagnostic;
  EXPECT_FALSE(Bad.Proof.Certificate);
  EXPECT_TRUE(Bad.Instructions.empty());
}

TEST(NativeUndefinedIndependence, NarrowControlTargetIsInvalidBeforePartition) {
  NativeProvider Provider;
  Provider.indirect(0x100, 0x20);
  Provider.ret(0x20);
  const auto Valid = neverd::analysis::detail::checkNativeUndefinedIndependence(
      Provider, {0x100}, contract(), {});
  ASSERT_TRUE(Valid.Proof.proved()) << Valid.Proof.Diagnostic;
  ASSERT_TRUE(Valid.Proof.Certificate.has_value());
  EXPECT_EQ(Valid.Proof.Paths, 1U);

  Provider.indirect(0x100, 0x20, 1);
  Provider.Fetches.clear();
  const auto Invalid =
      neverd::analysis::detail::checkNativeUndefinedIndependence(
          Provider, {0x100}, contract(), {});
  EXPECT_EQ(Invalid.Proof.Status, LowIRIndependenceStatus::Invalid);
  EXPECT_NE(Invalid.Proof.Diagnostic.find("64-bit address"), std::string::npos);
  EXPECT_EQ(Invalid.Proof.BlockVisits, 1U);
  EXPECT_EQ(Invalid.Proof.Operations, 1U);
  EXPECT_EQ(Provider.Fetches.size(), 1U);
  EXPECT_FALSE(Invalid.Proof.Certificate.has_value());
  EXPECT_TRUE(Invalid.Instructions.empty());
}

TEST(NativeUndefinedIndependence, IndirectCycleCannotPublishABoundedPrefix) {
  NativeProvider Provider;
  Provider.indirect(0x100, 0x200);
  Provider.ret(0x200);
  LowIRIndependenceLimits Limits;
  Limits.MaxPaths = 3;
  const auto Valid = neverd::analysis::detail::checkNativeUndefinedIndependence(
      Provider, {0x100}, contract(), Limits);
  ASSERT_TRUE(Valid.Proof.proved()) << Valid.Proof.Diagnostic;
  EXPECT_EQ(Valid.Proof.BlockVisits, 2U);

  // Only dynamic indirect edges close this cycle. Every destination has a
  // complete provider record, so missing bytes or a direct-graph cycle cannot
  // mask the requirement to exhaust the path budget without issuing a proof.
  Provider.indirect(0x200, 0x100);
  Provider.Fetches.clear();
  const auto Incomplete =
      neverd::analysis::detail::checkNativeUndefinedIndependence(
          Provider, {0x100}, contract(), Limits);
  EXPECT_EQ(Incomplete.Proof.Status, LowIRIndependenceStatus::BudgetExceeded);
  EXPECT_NE(Incomplete.Proof.Diagnostic.find("native path budget"),
            std::string::npos);
  EXPECT_EQ(Incomplete.Proof.BlockVisits, Limits.MaxPaths);
  EXPECT_EQ(Incomplete.Proof.Instructions, Limits.MaxPaths);
  EXPECT_EQ(Provider.Fetches.size(), 2U);
  EXPECT_EQ(Provider.Fetches.at(0x100), 1U);
  EXPECT_EQ(Provider.Fetches.at(0x200), 1U);
  EXPECT_FALSE(Incomplete.Proof.proved());
  EXPECT_FALSE(Incomplete.Proof.Certificate.has_value());
  EXPECT_TRUE(Incomplete.Instructions.empty());
  EXPECT_TRUE(Incomplete.Reads.empty());
}

TEST(NativeUndefinedIndependence, UntakenTrapStillRequiresExactEvidence) {
  NativeProvider Provider;
  Provider.untakenTrap();
  const auto Check = [&] {
    return neverd::analysis::detail::checkNativeUndefinedIndependence(
        Provider, {0x100}, contract(), {});
  };
  const auto Valid = Check();
  ASSERT_TRUE(Valid.Proof.proved()) << Valid.Proof.Diagnostic;
  ASSERT_EQ(Valid.Instructions.size(), 3U);
  EXPECT_EQ(Provider.Fetches.count(0x201), 0U);
  EXPECT_EQ(Valid.Instructions.back().UndefinedEffects.Coverage,
            LowUndefinedCoverage::Missing);
  const auto Original = Provider.Instructions.at(0x200);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto &Trap = Provider.Instructions.at(0x200);
    Trap = Original;
    switch (Mutation) {
    case 0:
      Trap.UndefinedEffects.OperationDigest.clear();
      break;
    case 1:
      Trap.Ops[0].Output = NdVar::reg(0, 8);
      break;
    case 2:
      Trap.Ops[0].Inputs[0] =
          NdVar::scalar(static_cast<uint64_t>(Intrinsic::Rdtsc), 2);
      break;
    case 3:
      Trap.Ops[0].addInput(NdVar::scalar(0, 1));
      break;
    case 4:
      Trap.Origin.ControlFlags = LowInstructionControlFlag::Terminator;
      break;
    case 5:
      Trap.NativeStackControl = SpecializationNativeStackControl::Return;
      break;
    case 6:
      Trap.Ops[0].Opcode = NdOp::NOP;
      Trap.Ops[0].NumInputs = 0;
      break;
    case 7:
      Trap.UndefinedEffects.OpCount = 0;
      break;
    }
    if (Mutation != 0)
      Trap.UndefinedEffects.OperationDigest =
          lowUndefinedOperationDigest(Trap.Ops);
    const auto Rejected = Check();
    EXPECT_TRUE(Rejected.Proof.Status == LowIRIndependenceStatus::Invalid ||
                Rejected.Proof.Status == LowIRIndependenceStatus::Unsupported)
        << Rejected.Proof.Diagnostic;
    EXPECT_FALSE(Rejected.Proof.Certificate.has_value());
    EXPECT_TRUE(Rejected.Instructions.empty());
    EXPECT_TRUE(Rejected.Reads.empty());
  }
}

TEST(NativeUndefinedIndependence,
     FlagTransitionCannotLaunderMalformedEvidence) {
  for (bool Pop : {false, true}) {
    SCOPED_TRACE(Pop);
    NativeProvider Provider;
    Provider.flags(Pop);
    auto Contract = contract();
    Contract.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
    const auto Check = [&] {
      return neverd::analysis::detail::checkNativeUndefinedIndependence(
          Provider, {0x100}, Contract, {});
    };
    const auto Valid = Check();
    ASSERT_TRUE(Valid.Proof.proved()) << Valid.Proof.Diagnostic;
    const auto Original = Provider.Instructions.at(0x100);
    for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto &Insn = Provider.Instructions.at(0x100);
      Insn = Original;
      auto &Op = Insn.Ops[0];
      switch (Mutation) {
      case 0:
        Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Missing;
        break;
      case 1:
        Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Unsupported;
        break;
      case 2:
        Insn.UndefinedEffects.OperationDigest.clear();
        break;
      case 3:
        ++Insn.UndefinedEffects.OpCount;
        break;
      case 4:
        Op.addInput(NdVar::scalar(0, 8));
        break;
      case 5:
        Op.Inputs[0].Size = 1;
        break;
      case 6:
        Op.Output = NdVar::reg(0, 8);
        break;
      case 7:
        if (Pop)
          Op.Inputs[1].Size = 2;
        else
          Op.Output.Size = 2;
        break;
      case 8:
        Op.Inputs[0].Offset += uint64_t{1} << 32;
        break;
      case 9:
        Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
        break;
      case 10:
        Insn.UndefinedEffects.Effects.push_back(
            {0, NdVar::reg(0, 8), 0, 1, {}});
        break;
      case 11:
        if (Pop)
          Op.Inputs[1] = NdVar::tmp(32, 8);
        else
          Op.Output.Offset = UINT64_MAX;
        break;
      case 12:
        Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        break;
      }
      if (Mutation != 2)
        Insn.UndefinedEffects.OperationDigest =
            lowUndefinedOperationDigest(Insn.Ops);
      const auto Result = Check();
      EXPECT_TRUE(Result.Proof.Status == LowIRIndependenceStatus::Invalid ||
                  Result.Proof.Status == LowIRIndependenceStatus::Unsupported)
          << Result.Proof.Diagnostic;
      EXPECT_FALSE(Result.Proof.Certificate.has_value());
      EXPECT_TRUE(Result.Instructions.empty());
      EXPECT_TRUE(Result.Reads.empty());
    }
  }
}

TEST(NativeUndefinedIndependence,
     ProfileProjectionNeedsExactIndependentEvidence) {
  NativeProvider Provider;
  Provider.rdssp();
  auto Contract = contract();
  const auto Check = [&] {
    return neverd::analysis::detail::checkNativeUndefinedIndependence(
        Provider, {0x100}, Contract, {});
  };
  EXPECT_EQ(Check().Proof.Status, LowIRIndependenceStatus::Unsupported);
  Contract.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
  const auto Valid = Check();
  ASSERT_TRUE(Valid.Proof.proved()) << Valid.Proof.Diagnostic;
  const auto Original = Provider.Instructions.at(0x100);
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto &Insn = Provider.Instructions.at(0x100);
    Insn = Original;
    switch (Mutation) {
    case 0:
      Insn.ProfileProjection = InterpreterProfileProjection::None;
      break;
    case 1:
      Insn.ProfileProjection = static_cast<InterpreterProfileProjection>(255);
      break;
    case 2:
      Insn.NativeBytes[4] = 0xc0;
      break; // Wrong /reg opcode extension.
    case 3:
      Insn.NativeBytes[4] = 0x08;
      break; // A memory operand is not RDSSP.
    case 4:
      Insn.NativeBytes[0] = 0xf2;
      break;
    case 5:
      Insn.NativeBytes[1] = 0x4c;
      break; // Uncertified prefix form.
    case 6:
      Insn.Ops[0].Output = NdVar::reg(0, 8);
      break;
    case 7:
      Insn.Ops[0].addInput(NdVar::scalar(0, 8));
      break;
    case 8:
      Insn.UndefinedEffects.OperationDigest.clear();
      break;
    case 9:
      Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Complete;
      break;
    case 10:
      Insn.UndefinedEffects.Effects.push_back({0, NdVar::reg(0, 8), 0, 1, {}});
      break;
    }
    if (Mutation != 8)
      Insn.UndefinedEffects.OperationDigest =
          lowUndefinedOperationDigest(Insn.Ops);
    const auto Result = Check();
    EXPECT_TRUE(Result.Proof.Status == LowIRIndependenceStatus::Invalid ||
                Result.Proof.Status == LowIRIndependenceStatus::Unsupported)
        << Result.Proof.Diagnostic;
    EXPECT_FALSE(Result.Proof.Certificate.has_value());
    EXPECT_TRUE(Result.Instructions.empty());
  }
}

TEST(NativeUndefinedIndependence,
     UnreachableProfileTrapStillRequiresExactEvidence) {
  NativeProvider Provider;
  Provider.untakenProfileTrap();
  auto Contract = contract();
  const auto Check = [&] {
    return neverd::analysis::detail::checkNativeUndefinedIndependence(
        Provider, {0x100}, Contract, {});
  };
  EXPECT_EQ(Check().Proof.Status, LowIRIndependenceStatus::Unsupported);
  Contract.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
  const auto Valid = Check();
  ASSERT_TRUE(Valid.Proof.proved()) << Valid.Proof.Diagnostic;
  const auto Original = Provider.Instructions.at(0x200);
  for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto &Insn = Provider.Instructions.at(0x200);
    Insn = Original;
    switch (Mutation) {
    case 0:
      Insn.ProfileProjection = InterpreterProfileProjection::None;
      break;
    case 1:
      Insn.ProfileProjection =
          InterpreterProfileProjection::CetDisabledReadShadowStackV1;
      break;
    case 2:
      Insn.NativeBytes[4] = 0xe0;
      break;
    case 3:
      Insn.NativeBytes[4] = 0x28;
      break;
    case 4:
      Insn.NativeBytes[0] = 0xf0;
      break;
    case 5:
      Insn.NativeBytes[1] = 0x4c;
      break;
    case 6:
      Insn.Ops[0].Inputs[0].Offset += uint64_t{1} << 32;
      break;
    case 7:
      Insn.Ops[0].Output = NdVar::tmp(0, 8);
      break;
    case 8:
      Insn.Ops[0].addInput(NdVar::scalar(0, 8));
      break;
    case 9:
      Insn.Origin.ControlFlags |= LowInstructionControlFlag::Resumable;
      break;
    case 10:
      Insn.UndefinedEffects.Coverage = LowUndefinedCoverage::Complete;
      break;
    case 11:
      Insn.UndefinedEffects.OperationDigest.clear();
      break;
    case 12:
      Insn.UndefinedEffects.Effects.push_back({0, NdVar::reg(0, 8), 0, 1, {}});
      break;
    case 13:
      Insn.Ops[0].Opcode = NdOp::NOP;
      Insn.Ops[0].NumInputs = 0;
      break;
    }
    if (Mutation != 11)
      Insn.UndefinedEffects.OperationDigest =
          lowUndefinedOperationDigest(Insn.Ops);
    const auto Result = Check();
    EXPECT_TRUE(Result.Proof.Status == LowIRIndependenceStatus::Invalid ||
                Result.Proof.Status == LowIRIndependenceStatus::Unsupported)
        << Result.Proof.Diagnostic;
    EXPECT_FALSE(Result.Proof.Certificate.has_value());
    EXPECT_TRUE(Result.Instructions.empty());
  }
}

} // namespace

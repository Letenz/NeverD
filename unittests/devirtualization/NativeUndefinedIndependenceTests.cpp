//===- NativeUndefinedIndependenceTests.cpp - Native proof boundaries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/NativeUndefinedIndependence.h"
#include "gtest/gtest.h"

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

} // namespace

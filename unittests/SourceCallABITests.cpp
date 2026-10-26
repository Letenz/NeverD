//===- SourceCallABITests.cpp - Source call carrier contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks platform placement and refuses incomplete call-state evidence.

#include "gtest/gtest.h"

#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"

#include <vector>
using namespace neverd;
namespace {
SourceCallTypeHint contract(std::vector<TypeRef> Types, int Fixed = -1) {
  SourceCallTypeHint H;
  H.CallKind = SourceCallTypeHint::Kind::Analyst;
  H.AnalystSite = SourceCallOccurrenceKey{0x1000, 3, NdOp::CALL, 0x2000};
  H.AnalystFixedCount = Fixed;
  H.Signature.ReturnType = NdType::makeInt(8);
  for (const auto &T : Types)
    H.Signature.Parameters.push_back({"", T});
  return H;
}
TEST(SourceCallABI, PlacesFixedScalarArgumentsByPlatform) {
  const auto Int = NdType::makeInt(4), Double = NdType::makeFloat(8),
             Pointer = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  for (auto A : {Arch::X64, Arch::AArch64})
    for (auto F :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      auto H = contract({Int, Double, Pointer});
      ASSERT_TRUE(assignAnalystCallABI(H, A, F, Error)) << Error;
      EXPECT_EQ(H.Signature.Parameters[1].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(H.Signature.Parameters[0].Location.ValueBytes, 4);
    }
  auto Windows = contract({Int, Double, Pointer, Int, Pointer});
  ASSERT_TRUE(
      assignAnalystCallABI(Windows, Arch::X64, BinaryFormat::COFF, Error))
      << Error;
  EXPECT_EQ(Windows.Signature.Parameters[4].Location.EntryStackOffset, 40);
  EXPECT_EQ(
      Windows.Signature.Parameters[1].Location.RegisterOffset,
      getTargetRegInfo(Arch::X64).floatingParamRegs(BinaryFormat::COFF)[1]);
}
TEST(SourceCallABI, WindowsARM64FixedScalarsKeepSeparateBanksAndStackSlots) {
  std::vector<TypeRef> Types;
  for (unsigned I = 0; I < 9; ++I) {
    Types.push_back(NdType::makeInt(4));
    Types.push_back(NdType::makeFloat(8));
  }
  auto Hint = contract(Types);
  Hint.Signature.ReturnType = NdType::makeFloat(8);
  std::string Error;
  ASSERT_TRUE(
      assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::COFF, Error))
      << Error;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned I = 0; I < 8; ++I) {
    const auto &Integer = Hint.Signature.Parameters[I * 2].Location;
    const auto &Floating = Hint.Signature.Parameters[I * 2 + 1].Location;
    EXPECT_EQ(Integer.Kind, SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Integer.RegisterOffset, TRI.IntParamRegs[I]);
    EXPECT_EQ(Integer.ValueBytes, 4);
    EXPECT_EQ(Floating.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Floating.RegisterOffset, TRI.FPParamRegs[I]);
    EXPECT_EQ(Floating.ValueBytes, 8);
  }
  for (unsigned I = 0; I < 2; ++I) {
    const auto &Stack = Hint.Signature.Parameters[16 + I].Location;
    EXPECT_EQ(Stack.Kind, SourceABICarrierKind::Stack);
    EXPECT_EQ(Stack.EntryStackOffset, I * 8);
    EXPECT_EQ(Stack.ValueBytes, I ? 8 : 4);
  }
  EXPECT_EQ(Hint.Signature.ReturnLocation.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint.Signature.ReturnLocation.RegisterOffset, TRI.FPReturnReg);
  EXPECT_EQ(Hint.Signature.ReturnLocation.ValueBytes, 8);

  // Windows varargs use the integer bank even for named floating operands.
  // Fixed-call placement must not silently admit that distinct convention.
  Hint = contract(Types, 1);
  EXPECT_FALSE(
      assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::COFF, Error));
  EXPECT_NE(Error.find("Windows ARM64 variadic"), std::string::npos);
}
TEST(SourceCallABI,
     RequiresPromotedVariadicsAndExplicitFloatingDuplicateSupport) {
  std::string Error;
  auto H = contract({NdType::makePtr(NdType::makeInt(1)), NdType::makeInt(4),
                     NdType::makeInt(8)},
                    1);
  EXPECT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error))
      << Error;
  H.Signature.Parameters[1].Type = NdType::makeInt(1);
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
  H.Signature.Parameters[1].Type = NdType::makeFloat(8);
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
  EXPECT_NE(Error.find("duplicate"), std::string::npos);
  EXPECT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::ELF, Error))
      << Error;
  EXPECT_TRUE(
      assignAnalystCallABI(H, Arch::AArch64, BinaryFormat::MachO, Error))
      << Error;
  EXPECT_EQ(H.Signature.Parameters[1].Location.Kind,
            SourceABICarrierKind::Stack);
}
TEST(SourceCallABI, NamedRecordPointersDoNotClaimRecordValueTransport) {
  auto Record = NdType::makeNamedRecord("DEVICE_OBJECT", 0);
  auto Pointer = NdType::makePtr(Record);
  auto H = contract({Pointer});
  H.Signature.ReturnType = Pointer;
  std::string Error;
  EXPECT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error))
      << Error;
  H.Signature.Parameters[0].Type = Record;
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
}
LowOp op(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}
TEST(SourceCallABI, VariadicFloatingNeedsTheActualVectorCount) {
  auto H = contract({NdType::makeInt(4), NdType::makeFloat(8)}, 1);
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::ELF, Error));
  LowFunc F;
  LowBlock B;
  B.Id = 0;
  B.Ops = {op(NdOp::COPY, NdVar::tmp(1, 4), {NdVar::cst(1, 4)}),
           op(NdOp::INT_ZEXT, NdVar::reg(0, 8), {NdVar::tmp(1, 4)}),
           op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  B.Ops.back().Addr = 0x1000;
  B.Ops.back().Seq = 3;
  F.Blocks = {B};
  EXPECT_TRUE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error))
      << Error;
  F.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0, 4);
  EXPECT_FALSE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error));
  F.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(9, 4);
  EXPECT_FALSE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error));
  F.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(1, 4);
  auto Earlier = op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)});
  Earlier.Addr = 0xff0;
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Earlier);
  EXPECT_FALSE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error));
  H.Signature.Parameters[0].Type = NdType::makeFloat(8);
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
}
TEST(SourceCallABI, RequiresCompleteSameBlockOutgoingStackBytes) {
  auto H = contract(std::vector<TypeRef>(5, NdType::makeInt(8)));
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
  const auto SP = NdVar::reg(getTargetRegInfo(Arch::X64).StackPointer, 8);
  LowFunc F;
  LowBlock B;
  B.Id = 0;
  B.Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::cst(64, 8)}),
           op(NdOp::INT_ADD, NdVar::tmp(1, 8), {SP, NdVar::cst(32, 8)}),
           op(NdOp::STORE, {}, {NdVar::tmp(1, 8), NdVar::cst(17, 8)}),
           op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  B.Ops.back().Addr = 0x1000;
  B.Ops.back().Seq = 3;
  F.Blocks = {B};
  ASSERT_TRUE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 8, Error))
      << Error;
  F.Blocks[0].Ops[2].Inputs[1].Size = 4;
  EXPECT_FALSE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 8, Error));
  F.Blocks[0].Ops[2].Inputs[1].Size = 8;
  EXPECT_FALSE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 0, Error));
  auto Earlier = op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)});
  Earlier.Addr = 0x0ff0;
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Earlier);
  EXPECT_FALSE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 8, Error));
}
MedVar value(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  return V;
}
TEST(SourceCallABI, RefusesClobberedAndUnseededPhiCarriers) {
  auto H = contract({NdType::makeInt(8)});
  MedFunc F;
  MedBlock B;
  B.Id = 0;
  MedOp D;
  D.Opcode = NdOp::COPY;
  D.Output = value(1);
  D.addInput(MedVar::makeConst(7, 8));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::make_shared<SourceCallTypeHint>(H);
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.addInput(value(1));
  B.Ops = {D, Call};
  F.Blocks = {B};
  std::string Error;
  EXPECT_TRUE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F.CallClobbers.push_back({value(1), 1});
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F.CallClobbers.clear();
  F.Blocks[0].Ops.erase(F.Blocks[0].Ops.begin());
  PhiNode Phi;
  Phi.Output = value(1);
  Phi.Args = {{0, value(1)}};
  F.Blocks[0].Phis = {Phi};
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
}
TEST(SourceCallABI, AcceptsOnlyTheExactPromotedEntryRegisterMarker) {
  auto H = contract({NdType::makeInt(8)});
  MedFunc F;
  MedBlock B;
  B.Id = 0;
  MedVar P = value(1);
  P.Kind = MedVar::Param;
  P.RegOff = 56;
  F.Params = {P};
  auto Input = P;
  Input.Kind = MedVar::Reg;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::make_shared<SourceCallTypeHint>(H);
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.addInput(Input);
  B.Ops = {Call};
  F.Blocks = {B};
  std::string Error;
  EXPECT_TRUE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error)) << Error;
  F.Blocks[0].Ops[0].Inputs[1].SSAVer = 1;
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
}
} // namespace

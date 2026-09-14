#include "../../../lib/pipeline/PipelineReturnModelingDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace {
using namespace neverd;

TEST(SourceABI, DarwinIntegerPairResultRequiresBothReturnRegisters) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
        << Diagnostic;
    EXPECT_TRUE(validateSourceABI(Hint, Diagnostic)) << Diagnostic;
    const auto &TRI = getTargetRegInfo(Architecture);
    ASSERT_EQ(Hint.ReturnComponents.size(), 2U);
    EXPECT_EQ(Hint.ReturnLocation.Kind, SourceABICarrierKind::None);
    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ(Hint.ReturnComponents[I].RegisterOffset, TRI.IntReturnRegs[I]);
      EXPECT_EQ(Hint.ReturnComponents[I].ValueBytes, 8U);
    }
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      auto Bad = Hint;
      switch (Mutation) {
      case 0:
        Bad.ReturnComponents.pop_back();
        break;
      case 1:
        Bad.ReturnComponents[1] = Bad.ReturnComponents[0];
        break;
      case 2:
        std::swap(Bad.ReturnComponents[0], Bad.ReturnComponents[1]);
        break;
      case 3:
        Bad.ReturnComponents[1].ValueBytes = 4;
        break;
      case 4:
        Bad.ReturnComponents[1].ExtendTo32Bits = true;
        break;
      case 5:
        Bad.ReturnComponents[1].EntryStackOffset = 8;
        break;
      case 6:
        Bad.ReturnComponents[1].Kind = SourceABICarrierKind::FloatingRegister;
        break;
      case 7:
        Bad.ReturnLocation = Bad.ReturnComponents[0];
        break;
      case 8:
        Bad.ReturnType = NdType::makeVoid();
        break;
      case 9:
        Bad.ReturnType = NdType::makeInt(8);
        break;
      }
      EXPECT_FALSE(validateSourceABI(Bad, Diagnostic)) << Mutation;
    }
    Hint.ReturnType = NdType::makeInt(8);
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic));
    EXPECT_TRUE(Hint.ReturnComponents.empty());
    Hint.Parameters = {{"wide", NdType::makeInt(16)}};
    EXPECT_FALSE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic));
  }
}

TEST(SourceABI, SourceReturnComponentsNeverBecomeRewriteABIEvidence) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    BinaryImage Image;
    Image.Arch = Architecture;
    const auto &TRI = getTargetRegInfo(Architecture);
    PipelineResult Result;
    Result.MedFuncs.resize(2);
    auto &Caller = Result.MedFuncs[0];
    auto &Callee = Result.MedFuncs[1];
    Caller.Entry = 0x1000;
    Callee.Entry = 0x2000;
    Caller.Blocks.resize(1);
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output.Kind = MedVar::Temp;
    Call.Output.Id = 10;
    Call.Output.Size = 16;
    Call.addInput(MedVar::makeConst(Callee.Entry, 8));
    auto Hint = std::make_shared<SourceCallTypeHint>();
    Hint->Signature.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(
        assignDarwinScalarSourceABI(Hint->Signature, Architecture, Diagnostic));
    Call.SourceCallHint = Hint;
    Caller.Blocks[0].Ops.push_back(Call);
    for (unsigned I = 0; I != 2; ++I) {
      MedOp Extract;
      Extract.Opcode = NdOp::SUBBYTES;
      Extract.Output.Kind = MedVar::Reg;
      Extract.Output.Id = 11 + I;
      Extract.Output.Size = 8;
      Extract.Output.RegOff = TRI.IntReturnRegs[I];
      Extract.addInput(Call.Output);
      Extract.addInput(MedVar::makeConst(I * 8, 4));
      Caller.Blocks[0].Ops.push_back(Extract);
    }
    recoverStructReturnFromCallers(Image, Result);
    EXPECT_TRUE(Callee.MultiReturn.empty());
    // Preserve the ordinary aggregate-remodeling path when no source hint
    // supplies the extracts; source annotation must be the deciding boundary.
    Caller.Blocks[0].Ops[0].SourceCallHint.reset();
    recoverStructReturnFromCallers(Image, Result);
    ASSERT_EQ(Callee.MultiReturn.size(), 2U);
    EXPECT_EQ(Callee.MultiReturn[1].RegOff, TRI.IntReturnRegs[1]);
  }
}

TEST(SourceABI, CallbackTypesKeepTheirSignaturesAndRejectMalformedGraphs) {
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  const auto Callback =
      NdType::makePtr(NdType::makeFunc(NdType::makeVoid(), {Pointer}));
  const auto Other = NdType::makePtr(NdType::makeFunc(
      NdType::makeVoid(), {NdType::makePtr(NdType::makeVoid())}));
  EXPECT_TRUE(equalSourceTypes(Callback, Other));
  EXPECT_FALSE(equalSourceTypes(Callback, Pointer));
  EXPECT_FALSE(equalSourceTypes(Callback, NdType::makePtr(NdType::makeFunc(
                                              NdType::makeInt(4), {Pointer}))));
  EXPECT_FALSE(equalSourceTypes(
      Callback, NdType::makePtr(NdType::makeFunc(NdType::makeVoid()))));
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = Callback;
    Hint.Parameters = {{"callback", Callback}, {"context", Pointer}};
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
        << Diagnostic;
    EXPECT_EQ(Hint.Parameters[0].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[0].Location.ValueBytes, 8U);
    for (const auto &Invalid :
         {NdType::makeFunc(nullptr),
          NdType::makeFunc(NdType::makeVoid(), {NdType::makeVoid()}),
          NdType::makeFunc(NdType::makeVoid(), {Callback->Pointee}),
          NdType::makeFunc(Callback->Pointee),
          NdType::makeFunc(NdType::makeVoid(),
                           std::vector<TypeRef>(65, Pointer))}) {
      Hint.Parameters[0].Type = NdType::makePtr(Invalid);
      EXPECT_FALSE(validateSourceABI(Hint, Diagnostic));
      EXPECT_FALSE(
          equalSourceTypes(Hint.Parameters[0].Type, Hint.Parameters[0].Type));
    }
    auto Cycle = NdType::makePtr();
    Cycle->Pointee = Cycle;
    Hint.Parameters[0].Type = Cycle;
    EXPECT_FALSE(validateSourceABI(Hint, Diagnostic));
    EXPECT_FALSE(equalSourceTypes(Cycle, Cycle));
    Cycle->Pointee.reset();
    auto Recursive = NdType::makeFunc(NdType::makeVoid());
    auto RecursivePointer = NdType::makePtr(Recursive);
    Recursive->ParamTypes.push_back(RecursivePointer);
    Hint.Parameters[0].Type = RecursivePointer;
    EXPECT_FALSE(validateSourceABI(Hint, Diagnostic));
    EXPECT_FALSE(equalSourceTypes(RecursivePointer, RecursivePointer));
    Recursive->ParamTypes.clear();
    auto TooDeep = Pointer;
    for (unsigned I = 0; I < 17; ++I)
      TooDeep = NdType::makePtr(TooDeep);
    EXPECT_FALSE(equalSourceTypes(TooDeep, TooDeep));
  }
}

SourceFunctionTypeHint declaration(TypeRef Result) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = std::move(Result);
  Hint.Parameters = {{"objc_self", NdType::makePtr()},
                     {"objc_cmd", NdType::makePtr()}};
  return Hint;
}

TEST(SourceABI, DarwinMixedArgumentsUseIndependentBanks) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeFloat(8));
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"arg0", NdType::makeInt(4)},
                            {"arg1", NdType::makeFloat(8)},
                            {"arg2", NdType::makeInt(8)},
                            {"arg3", NdType::makeFloat(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset, TRI.IntParamRegs[2]);
    EXPECT_EQ(Hint.Parameters[3].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[4].Location.RegisterOffset, TRI.IntParamRegs[3]);
    EXPECT_EQ(Hint.Parameters[5].Location.RegisterOffset, TRI.FPParamRegs[1]);
    EXPECT_EQ(Hint.Parameters[5].Location.ValueBytes, 4);
    EXPECT_EQ(Hint.ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Hint.ReturnLocation.RegisterOffset, TRI.FPReturnReg);
  }
}

TEST(SourceABI, DarwinStackLayoutPreservesNarrowArgumentOffsets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeInt(4));
    const auto Registers = getTargetRegInfo(Architecture).IntParamRegs.size();
    for (size_t I = 2; I < Registers; ++I)
      Hint.Parameters.push_back(
          {"arg" + std::to_string(I - 2), NdType::makeInt(4)});
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"byte", NdType::makeInt(1)},
                            {"short_value", NdType::makeInt(2)},
                            {"integer", NdType::makeInt(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    for (size_t I = 0; I != 3; ++I) {
      const auto &L = Hint.Parameters[Registers + I].Location;
      EXPECT_EQ(L.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(L.EntryStackOffset, Architecture == Arch::AArch64
                                        ? int64_t(I * 2)
                                        : int64_t(8 + I * 8));
    }
  }
}

TEST(SourceABI, FloatingBankOverflowDoesNotConsumeIntegerRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeFloat(8));
    for (unsigned I = 0; I != 9; ++I)
      Hint.Parameters.push_back(
          {"arg" + std::to_string(I), NdType::makeFloat(8)});
    Hint.Parameters.push_back({"integer", NdType::makeInt(8)});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    EXPECT_EQ(Hint.Parameters[10].Location.Kind, SourceABICarrierKind::Stack);
    EXPECT_EQ(Hint.Parameters[10].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 8 : 0);
    EXPECT_EQ(Hint.Parameters[11].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[2]);
  }
}

TEST(SourceABI, RejectsConflictingCarriersAndUnmodelledTypes) {
  auto Hint = declaration(NdType::makeInt(8));
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Error));
  auto Bad = Hint;
  Bad.Parameters[1].Location = Bad.Parameters[0].Location;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  EXPECT_NE(Error.find("share"), std::string::npos);
  Bad = Hint;
  Bad.ReturnLocation.Kind = SourceABICarrierKind::FloatingRegister;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  Bad = Hint;
  Bad.Parameters[0].Location.ValueBytes = 4;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  Bad = Hint;
  Bad.Parameters[0].Type = NdType::makeFloat(16);
  EXPECT_FALSE(assignDarwinObjCSourceABI(Bad, Arch::AArch64, Error));
  Bad = Hint;
  Bad.Parameters[0].Location = {SourceABICarrierKind::Stack, 0, 0, 8};
  Bad.Parameters[1].Location = {SourceABICarrierKind::Stack, 0, 0, 8};
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  EXPECT_NE(Error.find("Overlapping"), std::string::npos);
}

TEST(SourceABI, NarrowReturnExtensionRequiresAnExplicitDarwinArm64Carrier) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (const uint16_t Bytes : {1, 2, 4, 8}) {
      auto Hint = declaration(NdType::makeInt(Bytes));
      std::string Error;
      ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error));
      EXPECT_EQ(Hint.ReturnLocation.ExtendTo32Bits,
                Architecture == Arch::AArch64 && Bytes < 4);
      Hint.ReturnLocation.ExtendTo32Bits = true;
      EXPECT_EQ(validateSourceABI(Hint, Error),
                Architecture == Arch::AArch64 && Bytes < 4);
    }
  }
  auto Hint = declaration(NdType::makeInt(1));
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Error));
  Hint.Parameters[0].Location.ExtendTo32Bits = true;
  EXPECT_FALSE(validateSourceABI(Hint, Error));
  Hint.Parameters[0].Location.ExtendTo32Bits = false;
  Hint.ReturnType = NdType::makeFloat(4);
  Hint.ReturnLocation = {SourceABICarrierKind::FloatingRegister,
                         getTargetRegInfo(Arch::AArch64).FPReturnReg, 0, 4,
                         true};
  EXPECT_FALSE(validateSourceABI(Hint, Error));
  Hint.ReturnType = NdType::makeVoid();
  Hint.ReturnLocation = {};
  Hint.ReturnLocation.ExtendTo32Bits = true;
  EXPECT_FALSE(validateSourceABI(Hint, Error));
}

TEST(SourceABI, ExplicitSwiftReceiverCanUseDedicatedCalleeSavedRegister) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
    Hint.Architecture = Architecture;
    Hint.HasExplicitABI = true;
    Hint.ReturnType = NdType::makeInt(8);
    const auto &TRI = getTargetRegInfo(Architecture);
    Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                           TRI.IntReturnReg, 0, 8};
    // Swift class receivers use these dedicated registers; the validator must
    // not impose Objective-C's two hidden leading arguments.
    const auto Dedicated =
        Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
    Hint.Parameters = {
        {"self",
         NdType::makePtr(),
         {SourceABICarrierKind::IntegerRegister, Dedicated, 0, 8}}};
    std::string Error;
    EXPECT_TRUE(validateSourceABI(Hint, Error)) << Error;
  }
}

TEST(SourceABI, PackedGenericSlotReadsBindToDistinctSourceParameters) {
  auto Hint = declaration(NdType::makeInt(4));
  for (unsigned I = 0; I != 6; ++I)
    Hint.Parameters.push_back({"arg" + std::to_string(I), NdType::makeInt(4)});
  Hint.Parameters.insert(Hint.Parameters.end(), {{"arg6", NdType::makeInt(1)},
                                                 {"arg7", NdType::makeInt(2)},
                                                 {"arg8", NdType::makeInt(4)}});
  MedFunc Func;
  Func.Name = "packed";
  Func.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  for (unsigned I = 0; I != 3; ++I) {
    MedOp Read;
    Read.Opcode = I ? NdOp::SUBBYTES : NdOp::COPY;
    Read.Output.Kind = MedVar::Temp;
    Read.Output.Id = 20 + I;
    Read.Output.Size = uint16_t(1U << I);
    MedVar Slot;
    Slot.Kind = MedVar::Param;
    Slot.Id = 8;
    Slot.RegOff = kNoParamReg;
    Slot.Size = I ? 8 : 1;
    Read.addInput(Slot);
    if (I)
      Read.addInput(MedVar::makeConst(I * 2, 4));
    Block.Ops.push_back(Read);
  }
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Arch::AArch64);
  ASSERT_TRUE(Func.SourceTypeHint);
  ASSERT_TRUE(Func.SourceParametersBound);
  ASSERT_EQ(Func.Params.size(), 11U);
  for (unsigned I = 0; I != 3; ++I) {
    const auto &Read = Func.Blocks[0].Ops[I];
    EXPECT_EQ(Read.Inputs[0].Id, 8 + int(I));
    EXPECT_EQ(Read.Inputs[0].Size, 1U << I);
    if (I)
      EXPECT_EQ(Read.Inputs[1].ConstVal, 0U);
  }
  inferMedTypes(Func, Arch::AArch64);
  ASSERT_TRUE(Func.SourceTypeHint);
  EXPECT_EQ(Func.Blocks[0].Ops[2].Inputs[0].Id, 10);
  // An eight-byte read of this packed slot includes independent arguments and
  // padding; reject the projection instead of assigning it to the first one.
  Func.SourceParametersBound = false;
  Func.Blocks[0].Ops.resize(1);
  Func.Blocks[0].Ops[0].Inputs[0].Size = 8;
  Func.Blocks[0].Ops[0].Output.Size = 8;
  inferMedTypes(Func, Arch::AArch64);
  EXPECT_FALSE(Func.SourceTypeHint);
}

TEST(SourceABI, SourceStackExpressionsPreserveExactEntryOffsets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    auto Hint = declaration(NdType::makeInt(4));
    for (size_t I = 2; I < TRI.IntParamRegs.size(); ++I)
      Hint.Parameters.push_back(
          {"unused" + std::to_string(I), NdType::makeInt(8)});
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"byte", NdType::makeInt(1)},
                            {"short_value", NdType::makeInt(2)},
                            {"integer", NdType::makeInt(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    for (size_t I = 0; I != 3; ++I) {
      const auto Index = TRI.IntParamRegs.size() + I;
      const auto &Location = Hint.Parameters[Index].Location;
      const int64_t BaseOffset = Architecture == Arch::X64 ? 8 : 0;
      const auto SlotOffset = Location.EntryStackOffset - BaseOffset;
      MedFunc Func;
      Func.Name = "stack_parameter";
      Func.SourceTypeHint = Hint;
      Func.SourceTypeHint->ReturnType = Hint.Parameters[Index].Type;
      Func.SourceTypeHint->ReturnLocation.ValueBytes = Location.ValueBytes;
      MedVar Slot;
      Slot.Kind = MedVar::Param;
      Slot.TheArch = Architecture;
      Slot.Id = static_cast<int>(TRI.IntParamRegs.size() + SlotOffset / 8);
      Slot.RegOff = kNoParamReg;
      Slot.Size = SlotOffset % 8 ? 8 : Location.ValueBytes;
      MedOp Read;
      Read.Opcode = SlotOffset % 8 ? NdOp::SUBBYTES : NdOp::COPY;
      Read.Output.Kind = MedVar::Reg;
      Read.Output.TheArch = Architecture;
      Read.Output.Id = 20;
      Read.Output.RegOff = TRI.IntReturnReg;
      Read.Output.Size = Location.ValueBytes;
      Read.addInput(Slot);
      if (SlotOffset % 8)
        Read.addInput(MedVar::makeConst(SlotOffset % 8, 4));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Read.Output);
      MedBlock Block;
      Block.Id = 0;
      Block.Ops = {Read, Return};
      Func.Blocks.push_back(Block);
      inferMedTypes(Func, Architecture);
      ASSERT_TRUE(Func.SourceTypeHint);
      ASSERT_EQ(Func.Params[Index].RegOff, kNoParamReg);
      auto High = MedToHighConverter().convert(Func, Architecture);
      ASSERT_TRUE(High.SourceTypeHint);
      unsigned References = 0;
      auto Check = [&](auto &&Self, const ExprPtr &Expression) -> void {
        if (!Expression)
          return;
        if (Expression->Kind == ExprKind::Var &&
            Expression->Var.Kind == MedVar::Param) {
          ++References;
          EXPECT_EQ(Expression->Var.Id, int(Index));
          EXPECT_EQ(Expression->Var.StackOff, Location.EntryStackOffset);
          EXPECT_NE(Expression->Var.StackOff, -1);
          EXPECT_EQ(Expression->Var.Size, Location.ValueBytes);
        }
        for (const auto &Operand : Expression->Operands)
          Self(Self, Operand);
      };
      walkStmts(High.Body, [&](const HighStmt &Statement) {
        forEachRhsExpr(Statement, [&](const ExprPtr &Expression) {
          Check(Check, Expression);
        });
      });
      EXPECT_GT(References, 0U);
      EXPECT_EQ(Func.Params[Index].RegOff, kNoParamReg);
    }
  }
}

TEST(SourceABI, BitCastRejectsMismatchedWidthsAndDistinguishesTargetTypes) {
  auto Bits = HighExpr::makeConst(0x80000000, 4);
  auto Float = HighExpr::makeBitCast(Bits, NdType::makeFloat(4));
  auto Integer = HighExpr::makeBitCast(Bits, NdType::makeInt(4, true));
  EXPECT_EQ(Float->Kind, ExprKind::BitCast);
  EXPECT_FALSE(Float->structuralEq(*Integer));
  EXPECT_EQ(HighExpr::makeBitCast(Bits, NdType::makeFloat(8))->Kind,
            ExprKind::Undef);
  auto RoundTrip = HighExpr::makeBitCast(Float, Bits->Type);
  EXPECT_TRUE(RoundTrip->structuralEq(*Bits));
}

HighFunc scalarFloatFunction(Arch Architecture, uint16_t Width, bool Add) {
  MedFunc Func;
  Func.Name = std::string(Architecture == Arch::X64 ? "x64_" : "a64_") +
              (Width == 4 ? "f32_" : "f64_") + (Add ? "add" : "identity");
  auto Hint = declaration(NdType::makeFloat(Width));
  Hint.Parameters.push_back({"arg0", NdType::makeFloat(Width)});
  if (Add)
    Hint.Parameters.push_back({"arg1", NdType::makeFloat(Width)});
  Func.SourceTypeHint = Hint;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedBlock Block;
  Block.Id = 0;
  std::vector<MedVar> Incoming;
  for (unsigned I = 0; I != (Add ? 2U : 1U); ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Reg;
    Parameter.TheArch = Architecture;
    Parameter.Id = 20 + I;
    Parameter.RegOff = TRI.FPParamRegs[I];
    Parameter.Size = Architecture == Arch::X64 ? 16 : Width;
    MedOp Marker;
    Marker.Opcode = NdOp::COPY;
    Marker.Output = Parameter;
    Marker.addInput(Parameter);
    Block.Ops.push_back(Marker);
    Incoming.push_back(Parameter);
  }
  if (Add) {
    for (unsigned I = 0; I != 2; ++I) {
      MedOp Slice;
      Slice.Opcode = NdOp::SUBBYTES;
      Slice.Output.Kind = MedVar::Temp;
      Slice.Output.Id = 30 + I;
      Slice.Output.Size = Width;
      Slice.addInput(Incoming[I]);
      Slice.addInput(MedVar::makeConst(0, 4));
      Block.Ops.push_back(Slice);
    }
    MedOp Sum;
    Sum.Opcode = NdOp::FLOAT_ADD;
    Sum.Output.Kind = MedVar::Temp;
    Sum.Output.Id = 32;
    Sum.Output.Size = Width;
    Sum.addInput(Block.Ops[2].Output);
    Sum.addInput(Block.Ops[3].Output);
    Block.Ops.push_back(Sum);
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Output.Kind = MedVar::Reg;
    Widen.Output.Id = 33;
    Widen.Output.Size = 16;
    Widen.Output.RegOff = TRI.FPReturnReg;
    Widen.addInput(Sum.Output);
    Block.Ops.push_back(Widen);
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  // On x64 a generic return exposes RAX even when this declaration's result
  // is in XMM0. That unrelated integer carrier must not supply a float result.
  if (Architecture == Arch::X64) {
    MedVar RAX;
    RAX.Kind = MedVar::Reg;
    RAX.Id = 50;
    RAX.Size = 8;
    RAX.RegOff = TRI.IntReturnReg;
    Return.addInput(RAX);
  }
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Architecture);
  EXPECT_TRUE(Func.SourceTypeHint);
  auto High = MedToHighConverter().convert(Func, Architecture);
  EXPECT_TRUE(High.SourceTypeHint);
  EXPECT_EQ(High.ReturnType->Kind, NdTypeKind::Float);
  return High;
}

void executeC(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(FoundCompiler)) << "clang is required";
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-source-abi", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-abi", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-abi", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 16> Arguments{
        Compiler,   "-std=c11", Optimization, "-Werror=return-type",
        SourcePath, "-o",       BinaryPath};
    std::string Error;
    int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    llvm::SmallVector<llvm::StringRef, 1> RunArguments{BinaryPath};
    Status = llvm::sys::ExecuteAndWait(BinaryPath, RunArguments, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    EXPECT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

TEST(SourceABI, IntegerPairCallsPreserveBothWordsThroughSSAAndReturns) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto &TRI = getTargetRegInfo(Architecture);
    SourceFunctionTypeHint Pair;
    Pair.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Pair.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Pair, Architecture, Diagnostic));
    std::map<va_t, SourceFunctionTypeHint> Hints{{0x1100, Pair}};
    auto Scalar = Pair;
    Scalar.ReturnType = NdType::makeInt(8, false);
    ASSERT_TRUE(assignDarwinScalarSourceABI(Scalar, Architecture, Diagnostic));
    Hints[0x1150] = Scalar;
    std::vector<HighFunc> Functions;
    auto Operation = [](NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Opcode = Opcode;
      Op.Addr = 0x1200;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      return Op;
    };
    for (unsigned Mode = 0; Mode != 5; ++Mode) {
      LowFunc Low;
      Low.Entry = 0x1200;
      Low.Name = "pair_mode_" + std::to_string(Mode);
      LowBlock Block;
      Block.Id = 0;
      Block.StartAddr = Low.Entry;
      Block.EndAddr = 0x1220;
      Block.Ops.push_back(Operation(NdOp::CALL, NdVar::reg(TRI.IntReturnReg, 8),
                                    {NdVar::cst(0x1100, 8)}));
      auto Entry = Pair;
      if (Mode == 1) {
        Entry.ReturnType = NdType::makeInt(8, false);
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(Entry, Architecture, Diagnostic));
        Block.Ops.push_back(Operation(NdOp::COPY,
                                      NdVar::reg(TRI.IntReturnReg, 8),
                                      {NdVar::reg(TRI.IntReturnRegs[1], 8)}));
      } else if (Mode == 2) {
        Block.Ops.push_back(Operation(NdOp::COPY,
                                      NdVar::reg(TRI.IntReturnRegs[1], 8),
                                      {NdVar::cst(0x123456789abcdef0ULL, 8)}));
      } else if (Mode == 3) {
        // A later scalar call still destroys the second word. The source
        // declaration of the earlier call cannot make that clobber disappear.
        Block.Ops.push_back(Operation(NdOp::CALL,
                                      NdVar::reg(TRI.IntReturnReg, 8),
                                      {NdVar::cst(0x1150, 8)}));
      }
      Block.Ops.push_back(Operation(NdOp::RETURN, {}, {}));
      Low.Blocks.push_back(Block);
      if (Mode == 4) {
        Low.Blocks.resize(4);
        Low.Blocks[0].Ops.back() =
            Operation(NdOp::COND_BR, {},
                      {NdVar::cst(0x1400, 8), NdVar::reg(TRI.IntReturnReg, 1)});
        Low.Blocks[0].Succs = {1, 2};
        for (unsigned I = 1; I != 4; ++I) {
          auto &B = Low.Blocks[I];
          B.Id = I;
          B.StartAddr = 0x1200 + I * 0x100;
          B.EndAddr = B.StartAddr + 0x10;
          if (I != 3) {
            B.Preds = {0};
            B.Succs = {3};
            if (I == 2)
              B.Ops.push_back(Operation(NdOp::COPY,
                                        NdVar::reg(TRI.IntReturnRegs[1], 8),
                                        {NdVar::cst(17, 8)}));
            B.Ops.push_back(
                Operation(NdOp::BRANCH, {}, {NdVar::cst(0x1500, 8)}));
          } else {
            B.Preds = {1, 2};
            B.Ops.push_back(Operation(NdOp::RETURN, {}, {}));
          }
        }
      }
      Hints[Low.Entry] = Entry;
      LowToMedConverter Converter;
      Converter.setSourceCallHintsEnabled(true);
      Converter.setSourceCalleeTypeHints(&Hints);
      auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
      recoverCallAbi(Med, Architecture, {});
      Med.SourceTypeHint = Entry;
      inferMedTypes(Med, Architecture);
      auto High = MedToHighConverter().convert(Med, Architecture);
      EXPECT_TRUE(Med.MultiReturn.empty());
      if (Mode == 3) {
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
        EXPECT_NE(Source.find("caller-saved register clobbered"),
                  std::string::npos)
            << Source;
      } else {
        Functions.push_back(std::move(High));
      }
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    EXPECT_EQ(Source.find("caller-saved register clobbered"), std::string::npos)
        << Source;
    EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
    Source += R"(
static uint64_t low_word, high_word;
static unsigned calls;
unsigned __int128 sub_1100(void) {
  ++calls;
  return ((unsigned __int128)high_word << 64) | low_word;
}
int main(void) {
  for (unsigned i = 0; i != 4096; ++i) {
    low_word = UINT64_C(0x9e3779b97f4a7c15) * i;
    high_word = UINT64_C(0xfedcba9876543210) ^ ~low_word;
    unsigned __int128 expected = ((unsigned __int128)high_word << 64) | low_word;
    calls = 0;
    if (pair_mode_0() != expected || calls != 1) return 1;
    if (pair_mode_1() != high_word || calls != 2) return 2;
    expected = ((unsigned __int128)UINT64_C(0x123456789abcdef0) << 64) | low_word;
    if (pair_mode_2() != expected || calls != 3) return 3;
    expected = ((unsigned __int128)((uint8_t)low_word ? 17 : high_word) << 64) | low_word;
    if (pair_mode_4() != expected || calls != 4) return 4;
  }
  return 0;
}
)";
    ASSERT_NO_FATAL_FAILURE(executeC(Source));
  }
}

TEST(SourceABI, NarrowDarwinReturnsPreserveWordReadsWithoutInventingHighBits) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto ReturnReg = getTargetRegInfo(Architecture).IntReturnReg;
    const auto SavedReg =
        Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX;
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const uint16_t Bytes : {1, 2, 4}) {
      for (const bool Signed : {false, true}) {
        SCOPED_TRACE(Bytes);
        SCOPED_TRACE(Signed);
        const uint16_t KnownBytes = Architecture == Arch::AArch64 ? 4 : Bytes;
        SourceFunctionTypeHint Callee;
        Callee.ReturnType = NdType::makeInt(Bytes, Signed);
        std::string Error;
        ASSERT_TRUE(assignDarwinScalarSourceABI(Callee, Architecture, Error));
        std::map<va_t, SourceFunctionTypeHint> Hints{{0x1100, Callee}};
        std::string Source;
        llvm::raw_string_ostream OS(Source);
        std::vector<HighFunc> Functions;
        for (const uint16_t ReadBytes : {4, 8, 12, 16}) {
          LowFunc Low;
          Low.Entry = 0x1200;
          Low.Name = "narrow_read_" + std::to_string(ReadBytes);
          LowBlock Block;
          Block.Id = 0;
          Block.StartAddr = 0x1200;
          Block.EndAddr = 0x1208;
          LowOp Call;
          Call.Addr = 0x1200;
          Call.Opcode = NdOp::CALL;
          Call.Output = NdVar::reg(ReturnReg, 8);
          Call.addInput(NdVar::cst(0x1100, 8));
          LowOp Return;
          Return.Addr = 0x1204;
          Return.Opcode = NdOp::RETURN;
          Block.Ops = {Call};
          if (ReadBytes == 8) {
            // A full-register move preserves the known low word even though
            // the call's upper word is not part of the declared result.
            LowOp Copy;
            Copy.Addr = 0x1204;
            Copy.Opcode = NdOp::COPY;
            Copy.Output = NdVar::reg(SavedReg, 8);
            Copy.addInput(NdVar::reg(ReturnReg, 8));
            Block.Ops.push_back(Copy);
            Copy.Output = NdVar::reg(ReturnReg, 8);
            Copy.NumInputs = 0;
            Copy.addInput(NdVar::reg(SavedReg, 8));
            Block.Ops.push_back(Copy);
            Return.addInput(NdVar::reg(ReturnReg, KnownBytes));
          } else {
            Return.addInput(
                NdVar::reg(ReturnReg, ReadBytes == 16 ? 8 : KnownBytes));
          }
          Block.Ops.push_back(Return);
          Low.Blocks.push_back(Block);
          if (ReadBytes == 12) {
            // Merge a full saved register, then move it into the ABI result.
            // The return's low lane must reach back through that copy and
            // merge; its unused upper bits must not poison the source body.
            Low.Blocks.clear();
            Low.Blocks.resize(4);
            for (int I = 0; I < 4; ++I) {
              auto &B = Low.Blocks[I];
              B.Id = I;
              B.StartAddr = 0x1200 + I * 0x100;
              B.EndAddr = B.StartAddr + 0x20;
            }
            LowOp Branch;
            Branch.Opcode = NdOp::COND_BR;
            Branch.Addr = 0x1204;
            Branch.addInput(NdVar::cst(0x1400, 8));
            Branch.addInput(NdVar::reg(ReturnReg, 1));
            Low.Blocks[0].Ops = {Call, Branch};
            Low.Blocks[0].Succs = {1, 2};
            for (int I = 1; I <= 2; ++I) {
              auto &B = Low.Blocks[I];
              B.Preds = {0};
              B.Succs = {3};
              LowOp Copy;
              Copy.Opcode = NdOp::COPY;
              Copy.Addr = B.StartAddr;
              Copy.Output = NdVar::reg(SavedReg, 8);
              Copy.addInput(I == 1 ? NdVar::reg(ReturnReg, 8)
                                   : NdVar::cst(1, 8));
              LowOp Jump;
              Jump.Opcode = NdOp::BRANCH;
              Jump.Addr = B.StartAddr + 4;
              Jump.addInput(NdVar::cst(0x1500, 8));
              B.Ops = {Copy, Jump};
            }
            LowOp Copy;
            Copy.Opcode = NdOp::COPY;
            Copy.Addr = 0x1500;
            Copy.Output = NdVar::reg(ReturnReg, 8);
            Copy.addInput(NdVar::reg(SavedReg, 8));
            Return.Addr = 0x1504;
            Low.Blocks[3].Preds = {1, 2};
            Low.Blocks[3].Ops = {Copy, Return};
          }
          SourceFunctionTypeHint Entry;
          Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
          Entry.ReturnType =
              NdType::makeInt(ReadBytes == 16 ? 8 : KnownBytes,
                              Architecture == Arch::X64 && Signed);
          ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Architecture, Error));
          Hints[Low.Entry] = Entry;
          LowToMedConverter Converter;
          Converter.setSourceCallHintsEnabled(true);
          Converter.setSourceCalleeTypeHints(&Hints);
          auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
          recoverCallAbi(Med, Architecture, {{0x1100, "narrow_result"}});
          Med.SourceTypeHint = Entry;
          inferMedTypes(Med, Architecture);
          auto High = MedToHighConverter().convert(Med, Architecture);
          if (ReadBytes == 16) {
            std::string Unproven;
            llvm::raw_string_ostream UnprovenOS(Unproven);
            CEmitterOptions Options;
            Options.TheArch = Architecture;
            ASSERT_TRUE(HighCEmitter().emit({High}, UnprovenOS, Options));
            EXPECT_NE(Unproven.find("caller-saved register clobbered"),
                      std::string::npos)
                << Unproven;
          } else {
            Functions.push_back(std::move(High));
          }
        }
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
        OS.flush();
        EXPECT_EQ(Source.find("caller-saved register clobbered"),
                  std::string::npos)
            << Source;
        const std::string Type = std::string(Signed ? "int" : "uint") +
                                 std::to_string(Bytes * 8) + "_t";
        Source += "\nstatic uint32_t input;\n" + Type +
                  " sub_1100(void) { return (" + Type + ")input; }\n";
        Source += "int main(void) {\n"
                  "const uint32_t edges[] = {0x7fffffffU, 0x80000000U, "
                  "0xfffffffeU, 0xffffffffU};\n"
                  "for (unsigned round = 0; round != 65540; ++round) {\n"
                  "input = round < 65536 ? round : edges[round - 65536];\n"
                  "uint32_t expected = (uint32_t)(" +
                  Type +
                  ")input;\n"
                  "if ((uint32_t)narrow_read_4() != expected) return 1;\n"
                  "if ((uint32_t)narrow_read_8() != expected) return 2;\n"
                  "if ((uint32_t)narrow_read_12() != "
                  "((uint8_t)input ? 1U : expected)) return 3;\n"
                  "}\nreturn 0;\n}\n";
        ASSERT_NO_FATAL_FAILURE(executeC(Source));
      }
    }
  }
}

TEST(SourceABI,
     FloatProjectionExecutesNumericOperationsAndPreservesIdentityBits) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  std::string Checks;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    for (uint16_t Width : {4, 8})
      for (bool Add : {false, true})
        Functions.push_back(scalarFloatFunction(Architecture, Width, Add));
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    const std::string Prefix = Architecture == Arch::X64 ? "x64_" : "a64_";
    Checks +=
        "if (" + Prefix + "f32_add(0, 0, -7.5f, 2.25f) != -5.25f) return 1;\n";
    Checks += "if (" + Prefix +
              "f64_add(0, 0, -1024.5, 3.125) != -1021.375) return 2;\n";
    Checks +=
        "for (unsigned i = 0; i < sizeof(fbits)/sizeof(fbits[0]); ++i) {\n"
        "float v; memcpy(&v, &fbits[i], 4); float r = " +
        Prefix +
        "f32_identity(0, 0, v); uint32_t bits; memcpy(&bits, &r, 4);"
        "if (bits != fbits[i]) return 3; }\n";
    Checks +=
        "for (unsigned i = 0; i < sizeof(dbits)/sizeof(dbits[0]); ++i) {\n"
        "double v; memcpy(&v, &dbits[i], 8); double r = " +
        Prefix +
        "f64_identity(0, 0, v); uint64_t bits; memcpy(&bits, &r, 8);"
        "if (bits != dbits[i]) return 4; }\n";
  }
  OS.flush();
  Source += R"(
#include <string.h>
int main(void) {
  const uint32_t fbits[] = {0, 0x80000000U, 0x7f800000U, 0xff800000U,
                           0x7fc12345U, 1, 0xc0f00000U};
  const uint64_t dbits[] = {0, UINT64_C(0x8000000000000000),
                           UINT64_C(0x7ff0000000000000),
                           UINT64_C(0xfff0000000000000),
                           UINT64_C(0x7ff8000000001234), 1,
                           UINT64_C(0xc020800000000000)};
)" + Checks +
            "return 0;\n}\n";
  executeC(Source);
}

} // namespace

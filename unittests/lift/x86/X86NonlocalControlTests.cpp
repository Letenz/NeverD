//===- X86NonlocalControlTests.cpp - Nonlocal transfer admission ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Privilege and SMM transitions cannot use an ordinary function's return
/// or syscall contract. Exercise decoding and direct IR/source admission.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kEntry = 0x1000;

LowFunc lowFunction(Intrinsic Id, bool Result) {
  LowFunc Function;
  Function.Entry = kEntry;
  Function.Name = "nonlocal_transfer";
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.StartAddr = kEntry;
  Block.EndAddr = kEntry + 3;
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = kEntry;
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
  if (Result)
    Op.Output = NdVar::tmp(1, 8);
  Block.Ops.push_back(Op);
  LowInstructionBoundary Boundary;
  Boundary.Address = kEntry;
  Boundary.Size = 3;
  Boundary.OpCount = 1;
  Block.InstructionBoundaries.push_back(Boundary);
  return Function;
}

MedFunc medFunction(Intrinsic Id, bool Result) {
  MedFunc Function;
  Function.Entry = kEntry;
  Function.Name = "nonlocal_transfer";
  Function.ReturnType = NdType::makeVoid();
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = kEntry;
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = kEntry;
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 2));
  if (Result) {
    Op.Output.Kind = MedVar::Temp;
    Op.Output.Id = 1;
    Op.Output.Size = 8;
    Op.Output.SSAVer = 1;
  }
  Block.Ops.push_back(Op);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Return);
  return Function;
}

} // namespace

TEST(X86NonlocalControl, RawTransitionsCannotBecomeReturnsOrSyscalls) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (bool Fast : {false, true}) {
        Decoder D;
        ASSERT_TRUE(D.init(Target));
        D.setStrict(Strict);
        std::vector<std::vector<uint8_t>> Encodings{
            {0x0f, 0x34},
            {0x0f, 0x35},
            {0x0f, 0x07},
            {0x0f, 0xaa},
            {0xcf},
            {0xcb},
            {0xca, 8, 0},
            {0x66, 0xcf},
            {0xc7, 0xf8, 0, 0, 0, 0},
            {0x66, 0xc7, 0xf8, 0, 0},
            {0xc7, 0xf8, 0xfa, 0xff, 0xff, 0xff}};
        if (Target == Arch::X64)
          for (const auto &Encoding :
               std::vector<std::vector<uint8_t>>{{0x48, 0x0f, 0x35},
                                                 {0x48, 0x0f, 0x07},
                                                 {0x48, 0xcf},
                                                 {0x48, 0xcb}})
            Encodings.push_back(Encoding);
        for (const auto &Bytes : Encodings) {
          SCOPED_TRACE(testing::Message() << unsigned(Target) << Strict << Fast
                                          << unsigned(Bytes.back()));
          DecodedInsn Instruction{};
          ASSERT_EQ(Fast ? D.decodeOneForLift(Bytes.data(), Bytes.size(),
                                              kEntry, Instruction)
                         : D.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                       Instruction),
                    Bytes.size());
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(Instruction, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
        const uint8_t Ret = 0xc3;
        DecodedInsn Instruction{};
        ASSERT_EQ(D.decodeOneForLift(&Ret, 1, kEntry, Instruction), 1u);
        std::vector<LowOp> Ops;
        EXPECT_NO_THROW(D.liftToLow(Instruction, Ops));
        ASSERT_FALSE(Ops.empty());
        EXPECT_EQ(Ops.back().Opcode, NdOp::RETURN);
      }
}

TEST(X86NonlocalControl, LegacyIRRequiresTheMissingStateBoundary) {
  for (Intrinsic Id : {Intrinsic::Rsm, Intrinsic::Sysexitq, Intrinsic::Sysretq,
                       Intrinsic::Xbegin})
    for (bool Result : {false, true}) {
      SCOPED_TRACE(testing::Message() << unsigned(Id) << Result);
      const auto Low = lowFunction(Id, Result);
      auto Error = validateLowInstructionBoundaries(Low.Blocks.front());
      EXPECT_TRUE(bool(Error));
      if (Error)
        EXPECT_NE(llvm::toString(std::move(Error)).find("machine-state"),
                  std::string::npos);
      for (Arch Target : {Arch::X86, Arch::X64, Arch::AArch64}) {
        EXPECT_THROW(
            LowToMedConverter().convert(Low, Target, BinaryFormat::COFF),
            LowToMedConversionError);
        EXPECT_FALSE(verifyMedFunc(medFunction(Id, Result), "nonlocal"));
      }
    }
}

TEST(X86NonlocalControl, LLVMAdmissionAlsoChecksMaskedFunctions) {
  for (Intrinsic Id : {Intrinsic::Rsm, Intrinsic::Sysexitq, Intrinsic::Sysretq,
                       Intrinsic::Xbegin})
    for (bool Result : {false, true})
      for (bool Masked : {false, true}) {
        const auto Function = medFunction(Id, Result);
        llvm::LLVMContext Context;
        const std::vector<char> Mask{0};
        EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, Function.Name,
                                           Arch::X64, {}, nullptr,
                                           BinaryFormat::COFF, false,
                                           Masked ? &Mask : nullptr),
                     std::runtime_error);
      }
}

TEST(X86NonlocalControl, CRenderingCannotPublishBareAssembly) {
  for (Intrinsic Id : {Intrinsic::Rsm, Intrinsic::Sysexitq, Intrinsic::Sysretq,
                       Intrinsic::Xbegin})
    for (bool Gnu : {false, true}) {
      bool HasIntrinsics = false;
      EXPECT_THROW(renderX86IntrinsicCall(Id, {}, HasIntrinsics, Gnu),
                   std::invalid_argument);
      EXPECT_THROW(renderX86MultiOutput(Id, {}, {}, {}, {}, {}, Gnu),
                   std::invalid_argument);
      for (Arch Target : {Arch::X86, Arch::X64, Arch::AArch64}) {
        EXPECT_THROW(renderIntrinsicCall(Id, Target, {}, 0, HasIntrinsics, Gnu),
                     std::invalid_argument);
        EXPECT_THROW(MultiOutputRender{}(Target, Id, {}, {}, {}, {}),
                     std::invalid_argument);
        for (bool Hidden : {false, true}) {
          HighFunc Function;
          Function.Entry = kEntry;
          Function.Name = "nonlocal_transfer";
          Function.ReturnType = NdType::makeVoid();
          HighStmt Invoke;
          Invoke.Kind = StmtKind::ExprStmt;
          Invoke.Val = std::make_shared<HighExpr>();
          Invoke.Val->Kind = ExprKind::Call;
          Invoke.Val->Type = NdType::makeVoid();
          Invoke.Val->IntrinsicId = Id;
          if (Hidden) {
            HighStmt Branch;
            Branch.Kind = StmtKind::If;
            Branch.Cond = HighExpr::makeConst(0, 1);
            Branch.Body.push_back(Invoke);
            Function.Body.push_back(Branch);
          } else
            Function.Body.push_back(Invoke);
          CEmitterOptions Options;
          Options.TheArch = Target;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          EXPECT_THROW(HighCEmitter().emit({Function}, Out, Options),
                       std::invalid_argument);
          EXPECT_TRUE(Source.empty());
        }
      }
    }
}

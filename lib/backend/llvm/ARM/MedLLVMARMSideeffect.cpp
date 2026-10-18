//===- MedLLVMARMSideeffect.cpp - ARM (32-bit) side-effect intrinsics -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM (32-bit) side-effect intrinsic emission: barriers (DMB/DSB/ISB),
/// supervisor calls, breakpoints, exclusive monitors, coprocessor register
/// writes, and status register writes (MSR, CPS).
///
/// Value-producing intrinsics (RBIT) live in MedLLVMARMValueEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/MedLLVMInstructionText.h"

#define DEBUG_TYPE "neverd-med-llvm-arm-sideeffect"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Memory barriers (DMB, DSB, ISB)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitARMBarrier(const MedOp &Op, Intrinsic IC,
                                    llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::ArmDmb:
    // The strongest fence orders what any DMB option does; the option is
    // the text it carries (`dmb ishst`).
    markInstructionText(
        *Builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent), Op,
        IC);
    return true;
  case I::ArmDsb:
  case I::ArmIsb: {
    // A DSB or ISB runs with its own option: `dsb sy` waits for more than
    // `dsb ish` does.  A lift that kept none ran the inner-shareable one.
    std::string Text = intrinsicInstructionText(IC, instructionOperand(Op));
    if (Text.empty())
      Text = IC == I::ArmDsb ? "dsb ish" : "isb";
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, Text, "~{memory}", true);
    Builder.CreateCall(IA, {});
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Exception generators (SVC, HVC, SMC, BKPT, HLT, UDF, CLREX)
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitARMException(const MedOp &Op, Intrinsic IC,
                                      llvm::IRBuilder<> &Builder) {
  using I = Intrinsic;
  switch (IC) {
  case I::ArmSvc:
  case I::ArmHvc:
  case I::ArmSmc:
  case I::ArmBkpt:
  case I::ArmHlt:
  case I::ArmUdf: {
    const char *Mn = intrinsicAsmMnemonic(IC);
    if (Mn) {
      uint64_t Imm = 0;
      for (int K = 1; K < Op.NumInputs; ++K)
        if (Op.Inputs[K].isConst()) {
          Imm = Op.Inputs[K].ConstVal;
          break;
        }
      // The instruction with its code (`udf #0xFE`); a supervisor call keeps
      // the decimal number it has always printed.
      std::string AsmStr = intrinsicInstructionText(IC, Imm);
      if (AsmStr.empty())
        AsmStr = std::string(Mn) + " #" + std::to_string(Imm);
      auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
      auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
      auto *IA = llvm::InlineAsm::get(AsmFnTy, AsmStr, "~{memory}", true);
      Builder.CreateCall(IA, {});
    }
    return true;
  }
  case I::ArmClrex: {
    auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
    auto *AsmFnTy = llvm::FunctionType::get(VoidTy, {}, false);
    auto *IA = llvm::InlineAsm::get(AsmFnTy, "clrex", "~{memory}", true);
    Builder.CreateCall(IA, {});
    return true;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Top-level ARM side-effect dispatch
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitARMSideeffect(const MedOp &Op, Intrinsic IC,
                                       llvm::IRBuilder<> &Builder) {
  // A coprocessor register write runs as itself with its value (`mcr p15,
  // #0, $0, c7, c10, #5`), which is 64-bit for an MCRR.
  if (IC == Intrinsic::ArmMcr || IC == Intrinsic::ArmMcrr) {
    const std::string Asm =
        Op.NumInputs == 3 && Op.Inputs[1].isConst()
            ? armCoprocessorInstructionAsm(IC, Op.Inputs[1].ConstVal)
            : std::string();
    if (Asm.empty())
      llvm::report_fatal_error(
          "ARM coprocessor write without its coprocessor field");
    auto *ValueTy = sizeToType(IC == Intrinsic::ArmMcr ? 4 : 8);
    Builder.CreateCall(
        llvm::InlineAsm::get(llvm::FunctionType::get(
                                 llvm::Type::getVoidTy(*Ctx), {ValueTy}, false),
                             Asm, "r,~{memory}", /*hasSideEffects=*/true),
        {Builder.CreateZExtOrTrunc(getVar(Op.Inputs[2], Builder), ValueTy)});
    return true;
  }
  // An MSR runs as itself with the word it writes (`msr cpsr_c, $0`), and a
  // CPS alone (`cpsid i`).
  if (IC == Intrinsic::ArmMsr || IC == Intrinsic::ArmCps) {
    const bool Writes = IC == Intrinsic::ArmMsr;
    const std::string Asm =
        Op.NumInputs == (Writes ? 3 : 2) && Op.Inputs[1].isConst()
            ? armStatusInstructionAsm(IC, Op.Inputs[1].ConstVal)
            : std::string();
    if (Asm.empty())
      llvm::report_fatal_error("ARM status register write without its field");
    llvm::SmallVector<llvm::Value *, 1> Arguments;
    if (Writes)
      Arguments.push_back(Builder.CreateZExtOrTrunc(
          getVar(Op.Inputs[2], Builder), Builder.getInt32Ty()));
    auto *Type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*Ctx),
        Writes ? llvm::ArrayRef<llvm::Type *>(Builder.getInt32Ty())
               : llvm::ArrayRef<llvm::Type *>(),
        false);
    Builder.CreateCall(
        llvm::InlineAsm::get(Type, Asm,
                             Writes ? "r,~{memory},~{cpsr}" : "~{memory}",
                             /*hasSideEffects=*/true),
        Arguments);
    return true;
  }
  if (emitARMBarrier(Op, IC, Builder))
    return true;
  if (emitARMException(Op, IC, Builder))
    return true;
  return false;
}

} // namespace neverd

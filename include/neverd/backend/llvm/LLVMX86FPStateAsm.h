//===- LLVMX86FPStateAsm.h - Scalar FP completion assembly -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMX86FPSTATEASM_H
#define NEVERD_BACKEND_LLVM_LLVMX86FPSTATEASM_H

#include "neverd/ir/X86FPState.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"

#include <optional>
#include <string>

namespace neverd {

inline constexpr char X86FPStateAsmMetadata[] = "neverd.x86.fp-state";
inline constexpr char X86FPStateReadAsm[] = "stmxcsr ($0)";
inline constexpr char X86FPStateWriteAsm[] = "ldmxcsr ($0)";
inline constexpr char X86FPStateTransferConstraints[] = "r,~{memory}";
inline constexpr char X86FPStateBinaryConstraints[] = "=&x,0,x,r,~{memory}";
inline constexpr char X86FPStateConversionConstraints[] = "=&r,x,r,~{memory}";

inline std::string x86FPStateConversionAsm(Intrinsic Id, unsigned SourceBytes) {
  return "ldmxcsr ($2)\n\t" +
         std::string(x86FPStateConversionMnemonic(Id, SourceBytes)) +
         " $1,$0\n\tstmxcsr ($2)";
}

inline std::string x86FPStateBinaryAsm(Intrinsic Id, unsigned ScalarSize) {
  return "ldmxcsr ($3)\n\t" + std::string(x86ScalarFPStateMnemonic(Id)) +
         (ScalarSize == 4 ? "ss" : "sd") + " $2,$0\n\tstmxcsr ($3)";
}

/// Only owned, fully typed assembly contracts may acquire FP state source
/// helpers. A mnemonic alone cannot prove the operand roles or completion.
inline std::optional<std::pair<Intrinsic, unsigned>>
classifyX86FPStateAsm(const llvm::CallInst &Call) {
  if (!Call.getMetadata(X86FPStateAsmMetadata))
    return std::nullopt;
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm || !Asm->hasSideEffects() || Asm->isAlignStack() ||
      Asm->canThrow() || Asm->getDialect() != llvm::InlineAsm::AD_ATT)
    llvm::report_fatal_error("invalid x86 FP state assembly properties");
  const auto PointerIsDefault = [](const llvm::Value *Value) {
    return Value->getType()->isPointerTy() &&
           Value->getType()->getPointerAddressSpace() == 0;
  };
  if (Call.getType()->isVoidTy() && Call.arg_size() == 1 &&
      PointerIsDefault(Call.getArgOperand(0)) &&
      Asm->getConstraintString() == X86FPStateTransferConstraints) {
    if (Asm->getAsmString() == X86FPStateReadAsm)
      return std::pair{Intrinsic::X86ReadMXCSR, 0U};
    if (Asm->getAsmString() == X86FPStateWriteAsm)
      return std::pair{Intrinsic::X86WriteMXCSR, 0U};
  }
  for (Intrinsic Id : {Intrinsic::X86FPAddState, Intrinsic::X86FPSubState,
                       Intrinsic::X86FPMulState, Intrinsic::X86FPDivState})
    for (unsigned Bytes : {4U, 8U}) {
      const bool TypeMatches = Bytes == 4 ? Call.getType()->isFloatTy()
                                          : Call.getType()->isDoubleTy();
      if (TypeMatches && Call.arg_size() == 3 &&
          Call.getArgOperand(0)->getType() == Call.getType() &&
          Call.getArgOperand(1)->getType() == Call.getType() &&
          PointerIsDefault(Call.getArgOperand(2)) &&
          Asm->getConstraintString() == X86FPStateBinaryConstraints &&
          Asm->getAsmString() == x86FPStateBinaryAsm(Id, Bytes))
        return std::pair{Id, Bytes};
    }
  for (Intrinsic Id :
       {Intrinsic::X86FPCvtToIntState, Intrinsic::X86FPTruncToIntState})
    for (unsigned SourceBytes : {4U, 8U})
      for (unsigned DestinationBytes : {4U, 8U}) {
        if (!Call.getType()->isIntegerTy(DestinationBytes * 8) ||
            Call.arg_size() != 2 ||
            !(SourceBytes == 4
                  ? Call.getArgOperand(0)->getType()->isFloatTy()
                  : Call.getArgOperand(0)->getType()->isDoubleTy()) ||
            !PointerIsDefault(Call.getArgOperand(1)))
          continue;
        if (Asm->getConstraintString() == X86FPStateConversionConstraints &&
            Asm->getAsmString() == x86FPStateConversionAsm(Id, SourceBytes))
          return std::pair{
              Id, x86FPConversionLayout(SourceBytes, DestinationBytes)};
      }
  llvm::report_fatal_error("invalid x86 FP state assembly contract");
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMX86FPSTATEASM_H

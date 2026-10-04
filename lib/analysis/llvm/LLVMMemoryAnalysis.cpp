//===- LLVMMemoryAnalysis.cpp - Shared LLVM memory facts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/LLVMMemoryAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IntrinsicInst.h"

namespace neverd::analysis {

bool isLLVMMemoryTransparentIntrinsic(const llvm::Instruction &Instruction) {
  const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
  return Call && !Call->hasOperandBundles() && !Call->isConvergent() &&
         !Call->mayHaveSideEffects() && !Call->mayReadOrWriteMemory() &&
         llvm::isGuaranteedToTransferExecutionToSuccessor(Call);
}

std::optional<LLVMIntegerOffset>
splitLLVMIntegerOffset(llvm::Value *Value, llvm::APInt Offset,
                       llvm::function_ref<bool(uint64_t)> Charge) {
  if (!Value || !Value->getType()->isIntegerTy(Offset.getBitWidth()))
    return std::nullopt;
  for (;;) {
    if (!Charge(1))
      return std::nullopt;
    auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(Value);
    if (Binary && (Binary->getOpcode() == llvm::Instruction::Add ||
                   Binary->getOpcode() == llvm::Instruction::Sub)) {
      llvm::Value *Next = Binary->getOperand(0);
      auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
      if (!Constant && Binary->getOpcode() == llvm::Instruction::Add) {
        Constant = llvm::dyn_cast<llvm::ConstantInt>(Next);
        Next = Binary->getOperand(1);
      }
      if (Constant) {
        Offset += Binary->getOpcode() == llvm::Instruction::Add
                      ? Constant->getValue()
                      : -Constant->getValue();
        Value = Next;
        continue;
      }
    }
    return LLVMIntegerOffset{Value, std::move(Offset)};
  }
}

} // namespace neverd::analysis

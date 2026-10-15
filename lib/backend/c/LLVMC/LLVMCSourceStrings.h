//===- LLVMCSourceStrings.h - Explicit by-value string spelling -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep ordinary global address identity unless a call contract discards it.

#ifndef NEVERD_BACKEND_C_LLVMC_SOURCESTRINGS_H
#define NEVERD_BACKEND_C_LLVMC_SOURCESTRINGS_H

#include "neverd/backend/llvm/LLVMSourceTypeHints.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

namespace neverd {
inline const llvm::ConstantDataArray *
sourceStringArgument(const llvm::Value *Value, const llvm::CallBase &Call,
                     unsigned Index) {
  if (!Value->getType()->isPointerTy() ||
      !Call.getAttributes().hasParamAttr(Index,
                                         LLVMSourceByValueStringParameter))
    return nullptr;
  const auto *Global =
      llvm::dyn_cast<llvm::GlobalVariable>(Value->stripPointerCasts());
  if (!Global || !Global->hasLocalLinkage() || !Global->isConstant() ||
      !Global->hasInitializer() || Global->isExternallyInitialized() ||
      Global->getAddressSpace() != 0)
    return nullptr;
  const auto *Data =
      llvm::dyn_cast<llvm::ConstantDataArray>(Global->getInitializer());
  return Data && Data->isCString() ? Data : nullptr;
}

inline bool sourceStringOnlyCalledByValue(const llvm::GlobalVariable &Global) {
  if (Global.use_empty())
    return false;
  for (const auto &Use : Global.uses()) {
    const auto *Call = llvm::dyn_cast<llvm::CallBase>(Use.getUser());
    if (!Call || !Call->isArgOperand(&Use) ||
        !sourceStringArgument(&Global, *Call, Call->getArgOperandNo(&Use)))
      return false;
  }
  return true;
}
} // namespace neverd
#endif

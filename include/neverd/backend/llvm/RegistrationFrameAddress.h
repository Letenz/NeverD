//===- RegistrationFrameAddress.h - Checked PE32 byte addresses -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEADDRESS_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEADDRESS_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/Operator.h"

#include <cstdint>
#include <optional>

namespace neverd::registration_frame {

/// Restrict compiler-owned frame recipes to byte displacements whose signed
/// value survives PE32 index conversion. accumulateConstantOffset alone wraps
/// wide indices and element strides, and cannot prove GEP no-wrap promises.
inline std::optional<int64_t> checkedByteGEPOffset(const llvm::Value *Address) {
  const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Address);
  if (!GEP || GEP->getPointerAddressSpace() != 0 ||
      !GEP->getSourceElementType()->isIntegerTy(8) ||
      GEP->getNumIndices() != 1 || GEP->hasNoUnsignedWrap())
    return std::nullopt;
  const auto *Index = llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1));
  if (!Index || !Index->getValue().isSignedIntN(32))
    return std::nullopt;
  return Index->getValue().sextOrTrunc(32).getSExtValue();
}

} // namespace neverd::registration_frame
#endif

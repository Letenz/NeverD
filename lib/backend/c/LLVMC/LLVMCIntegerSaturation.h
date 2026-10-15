//===- LLVMCIntegerSaturation.h - Unsigned saturating arithmetic -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared scalar unsigned saturation admission for C emission.

#ifndef NEVERD_BACKEND_C_LLVM_C_INTEGERSATURATION_H
#define NEVERD_BACKEND_C_LLVM_C_INTEGERSATURATION_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <optional>
#include <stdexcept>

namespace neverd {
inline const char *integerSaturationSpelling(llvm::Intrinsic::ID Kind) {
  switch (Kind) {
  case llvm::Intrinsic::uadd_sat:
    return "uadd_sat";
  case llvm::Intrinsic::usub_sat:
    return "usub_sat";
  default:
    return nullptr;
  }
}
struct ScalarIntegerSaturation {
  unsigned Bits;
  bool Add;
};
/// One admission rule for helper declarations and assigned/inline calls.
inline std::optional<ScalarIntegerSaturation>
scalarIntegerSaturation(const llvm::CallBase &Call) {
  const auto *Callee = llvm::dyn_cast<llvm::Function>(
      Call.getCalledOperand()->stripPointerCasts());
  if (!Callee || !integerSaturationSpelling(Callee->getIntrinsicID()))
    return std::nullopt;
  const auto *Integer = llvm::dyn_cast<llvm::IntegerType>(Call.getType());
  const unsigned Bits = Integer ? Integer->getBitWidth() : 0;
  if (Call.getCalledFunction() != Callee || !llvm::isa<llvm::CallInst>(Call) ||
      Call.arg_size() != 2 ||
      (Bits != 1 && Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64 &&
       Bits != 128) ||
      Call.getArgOperand(0)->getType() != Integer ||
      Call.getArgOperand(1)->getType() != Integer ||
      Call.getFunctionType() != Callee->getFunctionType() ||
      Call.getFunctionType()->isVarArg())
    throw std::runtime_error("unsupported LLVM unsigned saturation shape");
  return ScalarIntegerSaturation{Bits, Callee->getIntrinsicID() ==
                                           llvm::Intrinsic::uadd_sat};
}
} // namespace neverd
#endif

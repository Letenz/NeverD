//===- LLVMCIntegerMinMax.h - Scalar integer min/max shapes -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVM_C_INTEGERMINMAX_H
#define NEVERD_BACKEND_C_LLVM_C_INTEGERMINMAX_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <optional>
#include <stdexcept>

namespace neverd {

inline const char *integerMinMaxSpelling(llvm::Intrinsic::ID Kind) {
  switch (Kind) {
  case llvm::Intrinsic::umin:
    return "umin";
  case llvm::Intrinsic::umax:
    return "umax";
  case llvm::Intrinsic::smin:
    return "smin";
  case llvm::Intrinsic::smax:
    return "smax";
  default:
    return nullptr;
  }
}

struct ScalarIntegerMinMax {
  unsigned Bits;
  bool Signed;
  bool Minimum;
};

/// Shared by helper declarations and every assigned/inline call projection.
/// Vector calls must first pass the emitter's ordinary scalarization guards.
inline std::optional<ScalarIntegerMinMax>
scalarIntegerMinMax(const llvm::CallBase &Call) {
  // getCalledFunction() hides a malformed direct call with a mismatched
  // callsite signature. Recognize it first so it cannot evade this guard.
  const auto *Callee = llvm::dyn_cast<llvm::Function>(
      Call.getCalledOperand()->stripPointerCasts());
  if (!Callee || !integerMinMaxSpelling(Callee->getIntrinsicID()))
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
    throw std::runtime_error("unsupported LLVM scalar integer min/max shape");
  const auto Kind = Callee->getIntrinsicID();
  return ScalarIntegerMinMax{
      Bits, Kind == llvm::Intrinsic::smin || Kind == llvm::Intrinsic::smax,
      Kind == llvm::Intrinsic::umin || Kind == llvm::Intrinsic::smin};
}

} // namespace neverd

#endif

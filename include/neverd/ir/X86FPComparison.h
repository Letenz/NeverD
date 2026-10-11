//===- X86FPComparison.h - Shared scalar comparison semantics --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One active lane's relation, DAZ and input-exception classification.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_X86FPCOMPARISON_H
#define NEVERD_IR_X86FPCOMPARISON_H
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <cstdint>
#include <optional>
namespace neverd {
struct X86FPComparisonResult {
  unsigned Relation; // LT, EQ, GT, UNORD.
  uint32_t Raised;
};
inline std::optional<X86FPComparisonResult>
evaluateX86FPComparison(uint64_t Left, uint64_t Right, unsigned Bytes,
                        bool Signaling, uint32_t MXCSR) {
  if (Bytes != 4 && Bytes != 8)
    return std::nullopt;
  const uint64_t Sign =
      Bytes == 4 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
  const uint64_t Exponent =
      Bytes == 4 ? UINT64_C(0x7f800000) : UINT64_C(0x7ff0000000000000);
  const uint64_t Fraction =
      Bytes == 4 ? UINT64_C(0x007fffff) : UINT64_C(0x000fffffffffffff);
  const uint64_t Quiet =
      Bytes == 4 ? UINT64_C(0x00400000) : UINT64_C(0x0008000000000000);
  const auto NaN = [&](uint64_t V) {
    return (V & Exponent) == Exponent && (V & Fraction);
  };
  if (NaN(Left) || NaN(Right))
    return X86FPComparisonResult{3, uint32_t(Signaling ||
                                             (NaN(Left) && !(Left & Quiet)) ||
                                             (NaN(Right) && !(Right & Quiet)))};
  const auto Denormal = [&](uint64_t V) {
    return !(V & Exponent) && (V & Fraction);
  };
  uint32_t Raised = 0;
  if (MXCSR & 0x40) {
    if (Denormal(Left))
      Left &= Sign;
    if (Denormal(Right))
      Right &= Sign;
  } else if (Denormal(Left) || Denormal(Right))
    Raised = 2;
  const auto &Semantics =
      Bytes == 4 ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble();
  const llvm::APFloat A(Semantics, llvm::APInt(Bytes * 8, Left));
  const llvm::APFloat B(Semantics, llvm::APInt(Bytes * 8, Right));
  switch (A.compare(B)) {
  case llvm::APFloat::cmpLessThan:
    return X86FPComparisonResult{0, Raised};
  case llvm::APFloat::cmpEqual:
    return X86FPComparisonResult{1, Raised};
  case llvm::APFloat::cmpGreaterThan:
    return X86FPComparisonResult{2, Raised};
  case llvm::APFloat::cmpUnordered:
    return std::nullopt;
  }
  return std::nullopt;
}
} // namespace neverd
#endif

//===- FloatingCallArguments.h - Scalar FP call carriers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The complete register transport of an all-scalar floating-point signature.
/// SourceParameterPlacement owns the target's parameter locations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_FLOATINGCALLARGUMENTS_H
#define NEVERD_IR_FLOATINGCALLARGUMENTS_H

#include "neverd/ir/SourceParameterPlacement.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

struct FloatingCallArguments {
  /// Scalar bytes in consecutive floating parameter registers.
  std::vector<uint16_t> ParameterBytes;
  uint16_t ReturnBytes = 0;
  bool operator==(const FloatingCallArguments &) const = default;
};

/// A complete fixed scalar FP signature carried only in FP registers.
/// Mixed classes, records, stack arguments, and special conventions require
/// other adapters. The caller must establish the ordinary fixed C convention.
std::optional<FloatingCallArguments>
floatingCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> Parameters);

} // namespace neverd

#endif // NEVERD_IR_FLOATINGCALLARGUMENTS_H

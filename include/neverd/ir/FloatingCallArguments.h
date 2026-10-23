//===- FloatingCallArguments.h - Scalar FP call carriers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The complete transport of a scalar signature with a floating-point result.
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
  struct ScalarParameter {
    SourceABICarrierKind Kind = SourceABICarrierKind::None;
    uint64_t Register = 0;
    int64_t StackOffset = 0;
    uint16_t Bytes = 0;
    bool Floating = false;
    bool Pointer = false;
    bool operator==(const ScalarParameter &) const = default;
  };
  /// A mixed scalar signature, in source order. Empty retains the consecutive
  /// floating-register representation above. Locations come exclusively from
  /// SourceParameterPlacement, including the entry-SP bias of stack values.
  std::vector<ScalarParameter> Scalars;
  bool operator==(const FloatingCallArguments &) const = default;
};

/// A complete fixed scalar signature. Consecutive floating registers use the
/// compact ParameterBytes form; mixed classes use source-ordered Scalars,
/// including stack locations. Records and special conventions need other
/// adapters. The caller must establish the ordinary fixed C convention.
std::optional<FloatingCallArguments>
floatingCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                      llvm::ArrayRef<TypeRef> Parameters);

} // namespace neverd

#endif // NEVERD_IR_FLOATINGCALLARGUMENTS_H

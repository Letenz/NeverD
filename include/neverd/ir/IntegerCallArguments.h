//===- IntegerCallArguments.h - Integer source-call carriers -----*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Map a declared integer/pointer signature, including records, onto the
/// integer-register and stack-slot operands recovered for a machine call.
/// SourceParameterPlacement owns the platform's placement rules; this adapter
/// gives argument recovery and source emission the same operand identities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTEGERCALLARGUMENTS_H
#define NEVERD_IR_INTEGERCALLARGUMENTS_H

#include "neverd/ir/SourceParameterPlacement.h"

namespace neverd {

struct IntegerCallArgumentPiece {
  unsigned Argument = 0;
  uint16_t Offset = 0;
  uint16_t Bytes = 0;
};

struct IntegerCallParameter {
  std::vector<IntegerCallArgumentPiece> Pieces;
  /// The sole operand points to the ABI's by-value copy of this parameter.
  bool Indirect = false;
};

struct IntegerCallArguments {
  std::vector<IntegerCallParameter> Parameters;
  /// The integer-register prefix actually read by this signature.
  unsigned Registers = 0;
  /// Recovered operand positions, including unused register positions before
  /// a stack argument. They are not a count of source parameters.
  unsigned Positions = 0;
};

/// Describe every parameter or return none. Floating carriers, hidden result
/// pointers, over-aligned stack values, unsupported conventions, and incomplete
/// placements are rejected.
/// The result uses recoverCallAbi's integer register/stack operand order.
std::optional<IntegerCallArguments>
integerCallArguments(Arch A, BinaryFormat F, const TypeRef &ReturnType,
                     llvm::ArrayRef<TypeRef> Parameters);

} // namespace neverd

#endif // NEVERD_IR_INTEGERCALLARGUMENTS_H

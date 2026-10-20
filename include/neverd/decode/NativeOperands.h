//===- NativeOperands.h - Typed decoded native operands ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Typed decoded native operands.
//===----------------------------------------------------------------------===//

#pragma once
#include "neverd/decode/Decoder.h"

namespace neverd {
struct NativeImmediate {
  unsigned Operand = 0;
  uint64_t Value = 0;
  unsigned Bits = 0;
};
struct NativeRegister {
  std::string Name;
  bool Read = false, Write = false;
};
struct NativeOperands {
  std::vector<NativeImmediate> Immediates;
  std::vector<NativeRegister> Registers;
};
/// Typed decoded operands, before classic text folding. Immediate bits are
/// logical operand bits, not the encoded immediate field's byte count.
NativeOperands describeNativeOperands(const Decoder &, const DecodedInsn &,
                                      Arch, bool IncludeRegisters = true);
} // namespace neverd

//===- X86NativeMemory.h - Native segment-offset roles --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep i386 native CS/ES/SS offsets scalar in MedIR and HighIR type inference.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_MED_X86_X86NATIVEMEMORY_H
#define NEVERD_IR_MED_X86_X86NATIVEMEMORY_H

#include "neverd/ir/intrinsics/X86/X86NativeMemory.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {
inline bool x86NativeMemoryHasSegmentOffset(const MedOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      Op.Inputs.size() < Op.NumInputs || !Op.Inputs[0].isConst())
    return false;
  const auto &Control = Op.Inputs[Op.NumInputs - 1];
  return Control.isConst() &&
         x86NativeMemoryHasSegmentOffset(Intrinsic(Op.Inputs[0].ConstVal),
                                         Op.NumInputs, Control.ConstVal);
}
} // namespace neverd
#endif // NEVERD_IR_MED_X86_X86NATIVEMEMORY_H

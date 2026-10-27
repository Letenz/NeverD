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

#include "neverd/ir/intrinsics/X86/X86CompressExpand.h"
#include "neverd/ir/intrinsics/X86/X86FourSource.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {
inline bool x86NativeMemoryHasSegmentOffset(const MedOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      Op.Inputs.size() < Op.NumInputs || !Op.Inputs[0].isConst())
    return false;
  const auto Id = Intrinsic(Op.Inputs[0].ConstVal);
  if (isX86CompactMemory(Id) && Op.NumInputs == 5 && Op.Inputs[4].isConst()) {
    const auto C = x86CompactControl(Id, Op.Inputs[4].ConstVal);
    return C && C->SegmentPrefix;
  }
  if (Id == Intrinsic::X86FourSourceMemory && Op.NumInputs == 6 &&
      Op.Inputs[5].isConst()) {
    const auto C = x86FourSourceControl(Op.Inputs[5].ConstVal);
    return C && C->SegmentPrefix;
  }
  return false;
}
} // namespace neverd
#endif // NEVERD_IR_MED_X86_X86NATIVEMEMORY_H

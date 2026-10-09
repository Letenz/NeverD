//===- X86FPStateShape.h - Typed HighIR state transports -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86FPSTATESHAPE_H
#define NEVERD_IR_HIGH_X86FPSTATESHAPE_H

#include "neverd/ir/X86FPState.h"
#include "neverd/ir/high/HighIR.h"

#include <algorithm>

namespace neverd {
inline X86FPStateShape x86FPStateHighShape(const HighExpr &Call,
                                           Arch TargetArch) {
  const auto Size = [&](unsigned Index) -> unsigned {
    return Index < Call.Operands.size() && Call.Operands[Index] &&
                   Call.Operands[Index]->Type
               ? Call.Operands[Index]->Type->Size
               : 0;
  };
  const bool Conversion = isX86FPConversionStateIntrinsic(Call.IntrinsicId);
  const bool HasSelector = Call.Operands.size() > 2 && Call.Operands[2];
  return {.TargetArch = TargetArch,
          .MemoryOrdering = Call.MemoryOrdering,
          .MemoryAddressSpace = Call.MemoryAddressSpace,
          .NumInputs = static_cast<unsigned>(Call.Operands.size() + 1),
          .IdIsConst = true,
          .IdSize = 2,
          .OutputIsWritable = Call.Type && Call.Type->Kind == NdTypeKind::Int &&
                              Call.Type->Size > 0,
          .OutputSize = Call.Type ? Call.Type->Size : 0U,
          .OperandsAreScalar =
              std::all_of(Call.Operands.begin(), Call.Operands.end(),
                          [](const auto &Operand) {
                            return Operand && Operand->Type &&
                                   (Operand->Type->Kind == NdTypeKind::Int ||
                                    Operand->Type->Kind == NdTypeKind::Float);
                          }),
          .LeftSize = Size(0),
          .RightSize = Size(1),
          .StateSize = Size(Conversion ? 1 : 2),
          .HasAuxiliaryOutputs = !Call.IntrinsicOutputs.empty(),
          .DestinationIsConst = HasSelector &&
                                Call.Operands[2]->Kind == ExprKind::Const &&
                                Call.Operands[2]->Type &&
                                Call.Operands[2]->Type->Kind == NdTypeKind::Int,
          .DestinationSelectorSize = Size(2),
          .DestinationBytes = HasSelector ? Call.Operands[2]->ConstVal : 0};
}
} // namespace neverd
#endif

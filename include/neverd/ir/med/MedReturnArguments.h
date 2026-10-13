//===- MedReturnArguments.h - Arguments a function returns ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The fields of a multi-register struct return that hold the arguments a
/// caller passed in the same registers.  `struct dd f(double a, double b)
/// {return (struct dd){a, b};}` compiles to a lone `ret`: no operation reads
/// or writes xmm0 and xmm1, yet each field is the argument passed there.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDRETURNARGUMENTS_H
#define NEVERD_IR_MED_MEDRETURNARGUMENTS_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <optional>

namespace neverd {

/// Whether an operation or phi of \p Func writes a byte of the \p Size bytes
/// at register offset \p RegOff.  A call may: the callee leaves its own
/// results in the caller-saved return registers (a getter that tail-calls its
/// superclass returns that call's record).  The copy that names an incoming
/// value at entry writes nothing.
inline bool writesRegisterBytes(const MedFunc &Func, uint64_t RegOff,
                                uint16_t Size) {
  auto Overlaps = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Size > 0 && V.RegOff < RegOff + Size &&
           RegOff < V.RegOff + V.Size;
  };
  for (const MedBlock &Block : Func.Blocks) {
    for (const auto &Phi : Block.Phis)
      if (Overlaps(Phi.Output))
        return true;
    for (const MedOp &Op : Block.Ops)
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          !Op.PreservesCallerSaved)
        return true;
      else if (Overlaps(Op.Output) &&
               !(Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
                 Op.Inputs[0] == Op.Output))
        return true;
  }
  return false;
}

/// The position of \p RegOff among the argument registers of its class, or
/// nothing when it carries no argument of that class.
inline std::optional<unsigned> argumentRegisterIndex(const TargetRegInfo &TRI,
                                                     BinaryFormat Format,
                                                     uint64_t RegOff,
                                                     bool IsFP) {
  if (IsFP) {
    const auto Registers = TRI.floatingParamRegs(Format);
    const auto It = std::find(Registers.begin(), Registers.end(), RegOff);
    if (It == Registers.end())
      return std::nullopt;
    return static_cast<unsigned>(It - Registers.begin());
  }
  const int Index = TRI.integerArgumentLayout(Format).registerIndex(RegOff);
  if (Index < 0)
    return std::nullopt;
  return static_cast<unsigned>(Index);
}

/// Whether \p Field of a multi-register struct return of \p Func is the
/// argument its caller passed in the same register: an argument register of
/// the field's class whose field bytes \p Func never writes.
inline bool returnsArgumentUnchanged(const MedFunc &Func,
                                     const MedReturnReg &Field,
                                     const TargetRegInfo &TRI,
                                     BinaryFormat Format) {
  return Field.Size > 0 &&
         argumentRegisterIndex(TRI, Format, Field.RegOff, Field.IsFP) &&
         !writesRegisterBytes(Func, Field.RegOff, Field.Size);
}

} // namespace neverd

#endif // NEVERD_IR_MED_MEDRETURNARGUMENTS_H

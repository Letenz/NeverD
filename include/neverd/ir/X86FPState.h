//===- X86FPState.h - Explicit x86 numerical and FP state results -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86FPSTATE_H
#define NEVERD_IR_X86FPSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

/// Scalar SSE operations consume raw operands and an explicit MXCSR value.
/// Their single result packs the numerical bits below the outgoing four-byte
/// MXCSR. SUBBYTES transports both definitions through SSA without implicit
/// auxiliary-output discovery. An unmasked exception prevents completion.
constexpr bool isX86ScalarFPStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPAddState || Id == Intrinsic::X86FPSubState ||
         Id == Intrinsic::X86FPMulState || Id == Intrinsic::X86FPDivState;
}

constexpr bool isX86FPStateIntrinsic(Intrinsic Id) {
  return isX86ScalarFPStateIntrinsic(Id) ||
         Id == Intrinsic::X86FPCvtToIntState ||
         Id == Intrinsic::X86FPTruncToIntState ||
         Id == Intrinsic::X86ReadMXCSR || Id == Intrinsic::X86WriteMXCSR;
}

constexpr bool isX86FPConversionStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPCvtToIntState ||
         Id == Intrinsic::X86FPTruncToIntState;
}

constexpr const char *x86FPStateConversionMnemonic(Intrinsic Id,
                                                   unsigned SourceBytes) {
  if (Id == Intrinsic::X86FPCvtToIntState)
    return SourceBytes == 4 ? "cvtss2si" : "cvtsd2si";
  if (Id == Intrinsic::X86FPTruncToIntState)
    return SourceBytes == 4 ? "cvttss2si" : "cvttsd2si";
  return nullptr;
}

constexpr const char *x86ScalarFPStateMnemonic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X86FPAddState:
    return "add";
  case Intrinsic::X86FPSubState:
    return "sub";
  case Intrinsic::X86FPMulState:
    return "mul";
  case Intrinsic::X86FPDivState:
    return "div";
  default:
    return nullptr;
  }
}

constexpr X86FPArithKind x86ScalarFPStateKind(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X86FPSubState:
    return X86FPArithKind::Subtract;
  case Intrinsic::X86FPMulState:
    return X86FPArithKind::Multiply;
  case Intrinsic::X86FPDivState:
    return X86FPArithKind::Divide;
  default:
    return X86FPArithKind::Add;
  }
}

struct X86FPStateShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  unsigned NumInputs = 0;
  bool IdIsConst = false;
  unsigned IdSize = 0;
  bool OutputIsWritable = false;
  unsigned OutputSize = 0;
  bool OperandsAreScalar = false;
  unsigned LeftSize = 0;
  unsigned RightSize = 0;
  unsigned StateSize = 0;
  bool HasAuxiliaryOutputs = false;
  bool DestinationIsConst = false;
  unsigned DestinationSelectorSize = 0;
  uint64_t DestinationBytes = 0;
  bool ArchitectureMatchesOperands = true;
};

constexpr bool x86FPStateShapeIsValid(Intrinsic Id,
                                      const X86FPStateShape &Shape) {
  if (!isX86FPStateIntrinsic(Id) || !Shape.ArchitectureMatchesOperands ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X86 &&
       Shape.TargetArch != Arch::X64) ||
      Shape.MemoryOrdering != NdMemoryOrdering::None ||
      Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Shape.IdIsConst || Shape.IdSize != 2 || Shape.HasAuxiliaryOutputs)
    return false;
  if (Id == Intrinsic::X86ReadMXCSR)
    return Shape.NumInputs == 1 && Shape.OutputIsWritable &&
           Shape.OutputSize == 4;
  if (Id == Intrinsic::X86WriteMXCSR)
    return Shape.NumInputs == 2 && Shape.OutputSize == 0 &&
           Shape.OperandsAreScalar && Shape.LeftSize == 4;
  if (isX86FPConversionStateIntrinsic(Id))
    return Shape.NumInputs == 4 && Shape.OutputIsWritable &&
           Shape.OperandsAreScalar &&
           (Shape.LeftSize == 4 || Shape.LeftSize == 8) &&
           Shape.StateSize == 4 && Shape.DestinationIsConst &&
           Shape.DestinationSelectorSize == 4 &&
           (Shape.DestinationBytes == 4 || Shape.DestinationBytes == 8) &&
           !(Shape.TargetArch == Arch::X86 && Shape.DestinationBytes == 8) &&
           Shape.OutputSize == Shape.DestinationBytes + 4;
  return Shape.NumInputs == 4 && Shape.OutputIsWritable &&
         Shape.OperandsAreScalar &&
         (Shape.LeftSize == 4 || Shape.LeftSize == 8) &&
         Shape.RightSize == Shape.LeftSize && Shape.StateSize == 4 &&
         Shape.OutputSize == Shape.LeftSize + 4;
}

/// Helper/classifier keys preserve independent source and integer widths.
/// Arithmetic retains its existing source-size key. Conversion uses the low
/// byte for the source width and the next byte for the destination width.
constexpr unsigned x86FPConversionLayout(unsigned SourceBytes,
                                         unsigned DestinationBytes) {
  return SourceBytes | (DestinationBytes << 8);
}
constexpr unsigned x86FPStateSourceBytes(unsigned Layout) {
  return Layout & 0xff;
}
constexpr unsigned x86FPStateDestinationBytes(Intrinsic Id, unsigned Layout) {
  return isX86FPConversionStateIntrinsic(Id) ? Layout >> 8 : Layout;
}
constexpr unsigned x86FPStateHelperLayout(Intrinsic Id,
                                          const X86FPStateShape &Shape) {
  return isX86FPConversionStateIntrinsic(Id)
             ? x86FPConversionLayout(
                   Shape.LeftSize,
                   static_cast<unsigned>(Shape.DestinationBytes))
         : isX86ScalarFPStateIntrinsic(Id) ? Shape.LeftSize
                                           : 0;
}

/// Only this exact slice of a completed aggregate denotes a scalar FP value.
/// The whole aggregate and its status slice remain integer bit carriers.
constexpr unsigned x86FPStateNumericalSliceSize(Intrinsic Id,
                                                const X86FPStateShape &Shape,
                                                uint64_t Offset,
                                                unsigned Bytes) {
  return isX86ScalarFPStateIntrinsic(Id) && x86FPStateShapeIsValid(Id, Shape) &&
                 Offset == 0 && Bytes == Shape.LeftSize
             ? Bytes
             : 0;
}

} // namespace neverd

#endif // NEVERD_IR_X86FPSTATE_H

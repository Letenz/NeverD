//===- AArch64WritebackMemory.h - LowIR writeback bindings -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// SIMD results require complete, independently stored temporary carriers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_AARCH64WRITEBACKMEMORY_H
#define NEVERD_IR_LOW_AARCH64WRITEBACKMEMORY_H

#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/low/LowIntrinsicOutputs.h"

#include <optional>
#include <vector>

namespace neverd {

/// Query a validated vector-load operation before conversion or tracing.
inline std::optional<std::vector<NdVar>>
lowAArch64VectorWritebackOutputs(const LowOp &Op,
                                 llvm::ArrayRef<LowOp> Following) {
  auto Outputs = collectLowIntrinsicOutputBindings(
      Op, Following,
      aarch64WritebackMemoryOutputCount(Intrinsic::A64_VectorLoadWriteback));
  if (!aarch64WritebackMemoryOutputsAreValid(
          Intrinsic::A64_VectorLoadWriteback, Outputs,
          [](const NdVar &V) { return V.Size; }) ||
      Outputs[kWritebackValueIndex].Offset ==
          Outputs[kWritebackBaseIndex].Offset)
    return std::nullopt;
  return Outputs;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_AARCH64WRITEBACKMEMORY_H

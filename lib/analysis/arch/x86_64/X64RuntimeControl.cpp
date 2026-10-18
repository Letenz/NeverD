//===- X64RuntimeControl.cpp - Checked x64 control dispatch -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dispatch runtime control operations without coupling independent owners.
//===----------------------------------------------------------------------===//

#include "X64ControlTransitions.h"
#include "X64Recovery.h"

namespace neverd::analysis::x64 {
llvm::Error
validateRuntimeControlOptions(const SpecializationOptions &Options) {
  return mxcsr::validateOptions(Options);
}

bool supportsRuntimeControlOperation(const LowOp &Op,
                                     const SpecializationOptions &Options) {
  return mxcsr::supportsOperation(Op, Options) ||
         alignment::supportsOperation(Op, Options);
}

RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx,
                        const SpecializationOptions &Options) {
  return mxcsr::seedState(Ctx, Options);
}

std::optional<RuntimeControlTransition>
lowerRuntimeControlOperation(const LowOp &Op, symbolic::SymState &State,
                             const RuntimeControlState &Controls,
                             const SpecializationOptions &Options,
                             llvm::function_ref<NdVar(uint16_t)> Temporary) {
  if (alignment::supportsOperation(Op, Options))
    return alignment::lowerOperation(Op, Options, Temporary);
  return mxcsr::lowerOperation(Op, State, Controls, Options, Temporary);
}
} // namespace neverd::analysis::x64

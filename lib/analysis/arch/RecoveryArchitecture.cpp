//===- RecoveryArchitecture.cpp - Native policy dispatch ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Explicit native recovery dispatch.

#include "../core/RecoveryArchitecture.h"

#include "x86_64/X64Recovery.h"

namespace neverd::analysis {
llvm::Error validateRuntimeMemoryOptions(const SpecializationOptions &Options) {
  if (Options.Architecture == Arch::X64)
    return x64::validateRuntimeMemoryOptions(Options);
  if (Options.X64SegmentedReads || Options.X64SegmentedWrites)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "segmented memory options require the x64 architecture");
  return llvm::Error::success();
}

bool supportsRuntimeMemoryOperation(const LowOp &Op,
                                    const SpecializationOptions &Options) {
  return Options.Architecture == Arch::X64 &&
         x64::supportsRuntimeMemoryOperation(Op, Options);
}

void seedRuntimeMemoryState(symbolic::SymContext &Ctx,
                            symbolic::SymState &State,
                            const SpecializationOptions &Options) {
  if (Options.Architecture == Arch::X64)
    x64::seedRuntimeMemoryState(Ctx, State, Options);
}

llvm::Expected<const RecoveryArchitecture *> recoveryArchitecture(Arch Target) {
  switch (Target) {
  case Arch::X64:
    return &x64::recoveryArchitecture();
  default:
    return llvm::createStringError(
        llvm::errc::not_supported,
        "interpreter recovery architecture %u has no adapter",
        static_cast<unsigned>(Target));
  }
}

llvm::Error
validateSpecializationExternalExits(const SpecializationResult &Result,
                                    const SpecializationOptions &Options) {
  auto Architecture = recoveryArchitecture(Options.Architecture);
  if (!Architecture)
    return Architecture.takeError();
  return (*Architecture)->validateExternalExits(Result, Options);
}
} // namespace neverd::analysis

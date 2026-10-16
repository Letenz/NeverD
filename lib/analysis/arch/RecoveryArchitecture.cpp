//===- RecoveryArchitecture.cpp - Native policy dispatch ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Explicit native recovery dispatch.

#include "../core/RecoveryArchitecture.h"

#include "x86_64/X64Recovery.h"

namespace neverd::analysis {
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

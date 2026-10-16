//===- RecoveryArchitecture.h - Recovery policy -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Native recovery contract dispatch.

#ifndef NEVERD_ANALYSIS_CORE_RECOVERYARCHITECTURE_H
#define NEVERD_ANALYSIS_CORE_RECOVERYARCHITECTURE_H

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/Support/Errc.h"

namespace neverd::analysis {
struct RawEnvironmentPreparation {
  uint64_t Bytes = 0;
  uint64_t Records = 0;
};

/// One allowance shared by generic receipt walks and native validation work.
struct RecoveryValidationBudget {
  uint64_t Bytes;
  uint64_t Records;
  bool bytes(uint64_t N) {
    if (N > Bytes)
      return false;
    Bytes -= N;
    return true;
  }
  bool records(uint64_t N, uint64_t Width) {
    if (N > Records || N > Bytes / Width)
      return false;
    Records -= N;
    Bytes -= N * Width;
    return true;
  }
  static llvm::Error exhausted() {
    return llvm::createStringError(
        std::make_error_code(std::errc::value_too_large),
        "raw environment result validation budget exhausted");
  }
};

/// Native policies live under arch/<isa>. Core owns graph/evidence integrity
/// and generic resource accounting; raw integrity never interprets native
/// fields.
class RecoveryArchitecture {
public:
  virtual ~RecoveryArchitecture() = default;
  virtual llvm::Expected<RawEnvironmentPreparation>
  validateRawDeclarations(const SpecializationOptions &Options) const = 0;
  virtual std::string
  rawOptionsDigest(const SpecializationOptions &Options) const = 0;
  virtual llvm::Error
  validateRawExtent(const SpecializationRawEnvironment &Domain) const = 0;
  virtual llvm::Error
  reserveExternalExitValidation(const SpecializationResult &Result,
                                const SpecializationOptions &Options,
                                RecoveryValidationBudget &Budget) const = 0;
  virtual std::string
  externalExitDigest(const SpecializationExternalExit &Exit) const = 0;
  virtual llvm::Error
  validateExternalExits(const SpecializationResult &Result,
                        const SpecializationOptions &Options) const = 0;
};

/// Selected from the declared target, never from the build host or receipt.
/// An architecture without an implementation refuses; it cannot borrow x64.
llvm::Expected<const RecoveryArchitecture *> recoveryArchitecture(Arch Target);
} // namespace neverd::analysis
#endif

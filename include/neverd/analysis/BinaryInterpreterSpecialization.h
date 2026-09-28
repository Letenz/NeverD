//===- BinaryInterpreterSpecialization.h - Image adapter --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BINARYINTERPRETERSPECIALIZATION_H
#define NEVERD_ANALYSIS_BINARYINTERPRETERSPECIALIZATION_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/analysis/LowIRUndefinedIndependence.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::analysis {

/// Specialize a linked x64 interpreter at the image's mapped addresses.
/// The execution contract fixes mappings and permissions, excludes concurrent
/// code/data mutation and external calls, and requires bytecode reads to come
/// from immutable file-backed mappings. This is source recovery, not a binary
/// replacement or an exception/unwind equivalence certificate.
/// Ordinary ABI return behavior is required: every external-origin store's
/// target range, including addresses computed from external integers, is
/// disjoint from the entry return-address slot. This is an environment
/// precondition, not a consequence of absent frame provenance. Root-derived
/// writes must prove disjointness; stack pivots and return dispatch are
/// refused in the default mode. ExplicitMachineState admits provider-certified
/// near calls and exact returns; NormalNonfaultingExecution excludes exception
/// dispatch rather than claiming its equivalence. X64CetDisabled certifies
/// RDSSP destination preservation, never arbitrary CET instruction support.
SpecializationResult
specializeBinaryInterpreter(const BinaryImage &Image, va_t Entry,
                            const SpecializationOptions &Options = {});

/// Evidence for the original, unpruned, direct-control acyclic leaf graph.
/// This is an undefined-state independence certificate, not a proof of the
/// lifter, a processor's undefined-bit choice, or native-to-source equivalence.
struct BinaryUndefinedIndependenceCertificate {
  /// Binds exact native bytes, their immutable mappings, execution profile and
  /// the LowIR certificate. Re-run the check to validate changed inputs; a hash
  /// alone is not a proof supplied by an untrusted party.
  std::string InputDigest;
  LowIRIndependenceCertificate LowIR;
  std::vector<SpecializationInstruction> Instructions;
};

struct BinaryUndefinedIndependenceResult {
  LowIRIndependenceResult Proof;
  std::optional<BinaryUndefinedIndependenceCertificate> Certificate;

  bool proved() const { return Proof.proved() && Certificate.has_value(); }
};

/// Collect both arms of every original direct branch before partial evaluation.
/// Missing bytes, overlapping instructions, calls, indirect transfers and
/// structural cycles refuse proof. Requires an explicit normal, nonfaulting,
/// CET-disabled x64 profile and a readable/writable frame disjoint from the
/// immutable image. Frame must be rooted at entry RSP and contain [0, 8).
/// The checker additionally proves entry RSP and the entry return slot are
/// restored on every return, before the final native return-address pop.
/// Entry constants and byte order must match Options; only Contract's stated
/// observations are certified. Limits bound collection as well as proof.
BinaryUndefinedIndependenceResult
checkBinaryUndefinedIndependence(const BinaryImage &Image, va_t Entry,
                                 const SpecializationOptions &Options,
                                 const LowIRIndependenceContract &Contract,
                                 const LowIRIndependenceLimits &Limits = {});

struct SpecializationWithIndependenceResult {
  SpecializationResult Recovery;
  BinaryUndefinedIndependenceResult Independence;
};

/// Require the independent original-graph proof before invoking recovery.
/// Refusal never contains residual code. A successful result retains separate
/// proof and recovery statuses; it does not certify the recovery transforms or
/// either C backend. Ordinary recovery remains available without this gate.
SpecializationWithIndependenceResult
specializeBinaryInterpreterWithIndependence(
    const BinaryImage &Image, va_t Entry, const SpecializationOptions &Options,
    const LowIRIndependenceContract &Contract,
    const LowIRIndependenceLimits &Limits = {});

} // namespace neverd::analysis

#endif

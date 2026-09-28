//===- BinaryInterpreterSpecialization.h - Image adapter --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_BINARYINTERPRETERSPECIALIZATION_H
#define NEVERD_ANALYSIS_BINARYINTERPRETERSPECIALIZATION_H

#include "neverd/analysis/InterpreterSpecialization.h"
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

} // namespace neverd::analysis

#endif

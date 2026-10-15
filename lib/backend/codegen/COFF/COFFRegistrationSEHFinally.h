//===- COFFRegistrationSEHFinally.h - SEH calls -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate ordinary finally calls against replayed source invocations.

#ifndef NEVERD_COFFREGISTRATIONSEHFINALLY_H
#define NEVERD_COFFREGISTRATIONSEHFINALLY_H

#include "COFFRegistrationIRProof.h"

namespace neverd::coff_registration {

/// Segments must come from validateSourceSegments: their function owners and
/// ordered call occurrences bind each invocation to its immutable source body.
llvm::Expected<std::set<const llvm::CallBase *>> bindSEHFinallyCalls(
    const MedFunc &Source, const llvm::Function &Parent,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::map<int, SourceSegment> &Segments,
    llvm::ArrayRef<const llvm::Function *> Functions);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHFINALLY_H

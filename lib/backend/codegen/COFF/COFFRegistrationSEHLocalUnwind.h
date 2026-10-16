//===- COFFRegistrationSEHLocalUnwind.h - CRT unwind proof ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares independent validation of native local-unwind bridge bodies.

#ifndef NEVERD_COFFREGISTRATIONSEHLOCALUNWIND_H
#define NEVERD_COFFREGISTRATIONSEHLOCALUNWIND_H

#include "neverd/ir/RegistrationCall.h"

#include "llvm/Support/Error.h"

namespace llvm {
class Function;
}

namespace neverd::coff_registration {

/// Check the bridge body independently of its creation. This grants no frame
/// capture or call-site authority; those must be bound to source occurrences.
llvm::Error
validateSEHLocalUnwindBridge(const llvm::Function &Bridge,
                             const RegistrationLocalUnwindContract &Source);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHLOCALUNWIND_H

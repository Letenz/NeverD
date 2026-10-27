//===- ControlFlowEntries.h - PE function entries -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_CONTROLFLOWENTRIES_H
#define NEVERD_LOADER_COFF_CONTROLFLOWENTRIES_H

#include "neverd/Common.h"

#include "llvm/Support/Error.h"

#include <vector>

namespace neverd {
struct BinaryImage;

/// Read the sorted PE GuardCFFunctionTable as function-entry evidence. A
/// complete immutable, raw-backed table, supported entry flags and executable
/// targets are required. Suppressed records supply no callable authority.
/// This proves neither an ABI nor the semantics of the function body, and does
/// not enable runtime CFG enforcement. Absent metadata returns an empty list;
/// malformed or unsupported metadata returns a diagnostic, never partial proof.
llvm::Expected<std::vector<va_t>>
readPEControlFlowEntries(const BinaryImage &Image);

} // namespace neverd

#endif // NEVERD_LOADER_COFF_CONTROLFLOWENTRIES_H

//===- MedCallReturnReads.h - Source call-stack observations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reject observations of physical call writes absent from a source frame.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLRETURNREADS_H
#define NEVERD_IR_MED_MEDCALLRETURNREADS_H

#include "neverd/ir/Limits.h"

#include <cstddef>
#include <string>

namespace neverd {
struct BinaryImage;
struct MedFunc;

/// Check exact entry-relative loads against reaching physical x64 CALL
/// return slots before source cleanup. An ordinary C call does not write the
/// reconstructed frame. A later explicit store kills only its own bytes;
/// reading any remaining call-written byte requires a machine-state source
/// contract. This check does not certify unresolved addresses or callee
/// effects.
bool validateSourceCallReturnReads(
    const MedFunc &Func, const BinaryImage &Image, std::string &Diagnostic,
    size_t WorkLimit = limits::ir::kSourceCallReturnReadWork);
} // namespace neverd

#endif // NEVERD_IR_MED_MEDCALLRETURNREADS_H

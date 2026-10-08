//===- MedStackAlignment.h - Proven entry-stack alignment -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDSTACKALIGNMENT_H
#define NEVERD_IR_MED_MEDSTACKALIGNMENT_H

#include "neverd/ir/med/MedIR.h"

#include <optional>

namespace neverd {

/// In source mode, simplify an alignment mask only when its operand is an
/// exact offset from an authenticated entry stack pointer and the target ABI
/// fixes every bit discarded by the mask for a function entered as \p Entry.
void simplifyProvenStackAlignment(MedFunc &Func, Arch Architecture,
                                  BinaryFormat Format,
                                  StackEntryKind Entry = StackEntryKind::Call);

/// The offset of \p Value from the authenticated entry stack pointer, when
/// copies, constant additions and proven alignment masks define it from that
/// pointer.
std::optional<int64_t>
entryStackOffset(const MedFunc &Func, const MedVar &Value, Arch Architecture,
                 BinaryFormat Format,
                 StackEntryKind Entry = StackEntryKind::Call);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDSTACKALIGNMENT_H

#ifndef NEVERD_IR_LOW_INDIRECTTAILFRAME_H
#define NEVERD_IR_LOW_INDIRECTTAILFRAME_H

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {

/// Necessary frame condition for the existing AArch64 indirect-tail heuristic.
/// Under the native call ABI, every reaching path must restore the incoming SP
/// and link word before this exact branch. This does not establish a callee,
/// source declaration, or jump-table domain. An incomplete proof grants
/// nothing.
std::set<va_t> restoredAArch64IndirectTailFrames(
    const LowFunc &Function, BinaryFormat Format,
    size_t MaxWork = limits::kMaxIndirectTailFrameWork);

} // namespace neverd
#endif

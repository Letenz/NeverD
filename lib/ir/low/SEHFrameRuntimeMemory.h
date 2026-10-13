//===- SEHFrameRuntimeMemory.h - Bound CRT invocations ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares runtime memory transfers used by the shared SEH frame proof.

#ifndef NEVERD_IR_LOW_SEHFRAMERUNTIMEMEMORY_H
#define NEVERD_IR_LOW_SEHFRAMERUNTIMEMEMORY_H

#include "SEHFrameState.h"

#include "neverd/ir/low/SEHRuntimeMemory.h"

#include <set>

namespace neverd {
struct TargetRegInfo;
namespace seh_frame {

/// Authenticate the current arguments of an exact runtime binding. A known
/// name with an unproved FILE, format, locale or va_list still refuses. The
/// invocation may use its Win64 home area and storage below SP. Neither the
/// physical CALL push nor the home area may replace an active return PC.
std::optional<State> transferRuntimeMemory(
    const LowOp &Call, const State &Caller, const TargetRegInfo &TRI,
    const LowSEHRuntimeMemory &Memory, const std::set<int64_t> &ReturnSlots);

bool imageStoreIsDisjoint(const LowSEHRuntimeMemory &Memory, va_t Address,
                          uint16_t Bytes);

} // namespace seh_frame
} // namespace neverd

#endif

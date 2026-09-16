#ifndef NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H
#define NEVERD_PIPELINE_NATIVESOURCEPRESERVATION_H

#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/low/LowIR.h"

#include <map>
#include <utility>

namespace neverd {

using NativeSourceCalls =
    std::map<std::pair<va_t, va_t>, const SourceFunctionTypeHint *>;

/// Prove that every exit restores the incoming Darwin preserved registers,
/// stack pointer and link register. Calls must already have validated source
/// declarations. Exact private spills may carry these identities across calls;
/// unknown writes invalidate spills; frame-address spills, escaping frame
/// values and incomplete graphs fail closed.
/// This does not prove a result type or authorize machine-code rewriting.
bool restoresNativeSourceState(const LowFunc &Function, Arch Architecture,
                               const NativeSourceCalls &Calls);

/// Prove the narrower frameless tail-call shape without requiring a synthetic
/// frame reconstruction. The function may not write any preserved, frame,
/// stack, or link register; every native call must be an immediate tail call.
/// A bounded byte-taint proof additionally rejects passing or storing a value
/// derived from the incoming stack pointer. This preserves the established
/// leaf contract while closing frame-address escape through caller-save
/// registers.
bool preservesNativeSourceLeafState(const LowFunc &Function, Arch Architecture,
                                    const NativeSourceCalls &Calls);

} // namespace neverd
#endif

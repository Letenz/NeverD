#ifndef NEVERD_LOADER_SWIFT_SWIFTOPAQUEVALUEEFFECTS_H
#define NEVERD_LOADER_SWIFT_SWIFTOPAQUEVALUEEFFECTS_H

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/low/SourceFrameEffects.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Independently authenticate a bounded typed value operation at a current
/// original ARM64 BL. Native copy/destroy require the complete current callee
/// LowIR, immutable machine body, metadata import and physical ABI. Equality
/// requires the current strong runtime import and its complete Boolean binding;
/// its one-bit result proof remains the Boolean owner's responsibility.
///
/// Effects describe typed Initialize/Read/Destroy, not initialized padding.
/// Return aliases are possible frame provenance, never frame byte identities.
/// This does not publish a source call, infer an entry ABI, replace a dynamic
/// value witness, or prove that any particular caller frame satisfies a borrow.
std::optional<SourceFrameEffects>
swiftOpaqueValueCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                            const SourceCallOccurrenceKey &Site,
                            const SourceCallTypeHint &Binding,
                            const LowFunc *Callee = nullptr);

/// Rebuild occurrence receipts using current, complete callee bodies. Only
/// callers with an authenticated native value operation receive typed read
/// receipts for their equality calls. Ordinary Boolean consumers are unchanged.
std::map<va_t, SourceCallTypeHint> buildSwiftOpaqueValueCallHints(
    const BinaryImage &Image, const LowFunc &Caller,
    const std::map<va_t, SourceFunctionTypeHint> &CalleeABIs,
    const std::map<va_t, const LowFunc *> &Callees);
} // namespace neverd
#endif

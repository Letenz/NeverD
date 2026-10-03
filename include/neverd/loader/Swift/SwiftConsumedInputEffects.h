#ifndef NEVERD_LOADER_SWIFT_SWIFTCONSUMEDINPUTEFFECTS_H
#define NEVERD_LOADER_SWIFT_SWIFTCONSUMEDINPUTEFFECTS_H

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/low/SourceFrameEffects.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Recognize a possible target without trusting a persisted declaration.
/// This does not grant a type-specific input effect.
bool isSwiftConsumedInputCallTarget(const BinaryImage &Image, va_t Target);

/// Current machine/LowIR call and exact strong type/witness identities. Only
/// the independently compiler-proved UInt or ObjectIdentifier temporary
/// contract qualifies, never an arbitrary
/// instantiation of the same generic initializer. No frame or source gate.
std::map<va_t, SourceCallTypeHint>
buildSwiftConsumedInputCallHints(const BinaryImage &Image,
                                 const LowFunc &Caller);

/// Revalidate the complete current receipt and ABI. The IR frame owner must
/// prove the eight initialized, nonescaping bytes and reject reads after the
/// consuming call until they are definitely rewritten. External call effects
/// and the opaque indirect result remain observable.
std::optional<SourceFrameEffects>
swiftConsumedInputCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                              const SourceCallTypeHint &Binding);
} // namespace neverd
#endif

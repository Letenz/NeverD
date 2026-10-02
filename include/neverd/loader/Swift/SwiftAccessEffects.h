#ifndef NEVERD_LOADER_SWIFT_SWIFTACCESSEFFECTS_H
#define NEVERD_LOADER_SWIFT_SWIFTACCESSEFFECTS_H

#include "neverd/ir/low/SourceCallOccurrence.h"
#include "neverd/ir/low/SourceFrameEffects.h"

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct SourceCallTypeHint;

/// Recognize the current import independently of a persisted call declaration.
/// This grants no frame effects or source-publication authority.
bool isSwiftAccessCallTarget(const BinaryImage &Image, va_t Target);

/// Authenticate the original ARM64 BL, strong import and complete current ABI.
/// beginAccess requires an exact Read/Modify flag, with or without Tracking.
/// Tracked scratch is retained in TLS: the shared frame proof must match its
/// end on every path before the frame expires. endAccess requires the same
/// opaque initialized record and may update other live runtime-owned links.
std::optional<SourceFrameEffects>
swiftAccessCallEffects(const BinaryImage &Image, const LowFunc &Caller,
                       const SourceCallOccurrenceKey &Site,
                       const SourceCallTypeHint &Binding);
} // namespace neverd
#endif

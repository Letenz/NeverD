#ifndef NEVERD_PIPELINE_NATIVE_SOURCE_OUTPUT_FRAME_H
#define NEVERD_PIPELINE_NATIVE_SOURCE_OUTPUT_FRAME_H

#include "neverd/ir/low/SourceFrameEffects.h"

namespace neverd {
struct BinaryImage;
struct SourceCallTypeHint;
struct NativeSourceCalleeContracts;
namespace detail {
// Replays current immutable machine/CFG, callee ABI and SDK occurrences.
// This supplies a private-frame write certificate, never source publication.
std::optional<SourceFrameEffects>
nativeOutputFrameEffects(const BinaryImage &Image,
                         const SourceCallTypeHint &Binding,
                         const NativeSourceCalleeContracts *Callees);
} // namespace detail
} // namespace neverd
#endif

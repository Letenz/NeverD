#ifndef NEVERD_LOADER_SWIFT_SWIFTRUNTIMECALLS_H
#define NEVERD_LOADER_SWIFT_SWIFTRUNTIMECALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Bind a nonconflicting Mach-O import slot to a declared fixed C or Swift
/// runtime ABI. A symbol spelling alone never authenticates a local function or
/// a veneer.
std::optional<SourceCallTypeHint>
swiftRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

/// Independent generic compiler evidence that this exact imported conformance
/// never consumes swift_getWitnessTable's instantiation-arguments pointer.
/// The metadata remains the caller's value; no layout or memory effects follow.
bool swiftWitnessInstantiationArgumentUnused(const BinaryImage &Image,
                                             va_t DescriptorSlot);

} // namespace neverd
#endif

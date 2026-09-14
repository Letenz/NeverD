#ifndef NEVERD_LOADER_MACHO_DARWINRUNTIMECALLS_H
#define NEVERD_LOADER_MACHO_DARWINRUNTIMECALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// A source binding for an exact platform import with a declared Darwin C ABI.
/// The original lock storage and all calls remain observable.
std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot);
} // namespace neverd

#endif

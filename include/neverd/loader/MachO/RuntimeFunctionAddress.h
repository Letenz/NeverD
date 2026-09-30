#ifndef NEVERD_LOADER_MACHO_RUNTIMEFUNCTIONADDRESS_H
#define NEVERD_LOADER_MACHO_RUNTIMEFUNCTIONADDRESS_H

#include "neverd/ir/SourceCallTypeHint.h"

namespace neverd {
struct BinaryImage;

/// Authenticate a strong runtime import and its complete, fixed ordinary C
/// declaration. The result names the loaded function's identity, never the
/// import cell's address or a copied code/data object. Swift-call functions,
/// variadic declarations and unsupported type shapes remain unbound.
std::optional<SourceCallTypeHint>
runtimeCFunctionAddressHint(const BinaryImage &Image, va_t ImportSlot);
} // namespace neverd
#endif

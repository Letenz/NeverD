#ifndef NEVERD_LOADER_MACHO_DARWINRUNTIMECALLS_H
#define NEVERD_LOADER_MACHO_DARWINRUNTIMECALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// A source binding for an exact platform import with a declared Darwin C ABI.
/// Storage, checks, and runtime calls remain observable.
std::optional<SourceCallTypeHint>
darwinRuntimeSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

struct DarwinFormatDeclaration {
  SourceFunctionTypeHint Signature;
  std::string Name;
  unsigned FormatParameter = 0;
};

/// Fixed prefix and NSString format attribute of an exact C import. This
/// declaration alone cannot bind a variadic call's actual arguments.
std::optional<DarwinFormatDeclaration>
darwinRuntimeFormatDeclaration(const BinaryImage &Image, va_t ImportSlot);

std::optional<SourceCallTypeHint>
darwinFormattedSourceCallHint(const BinaryImage &Image, va_t ImportSlot,
                              va_t FormatAddress);

/// Address supplied by an exact data import with a known platform contract.
std::optional<SourceCallTypeHint>
darwinRuntimeGlobalAddressHint(const BinaryImage &Image, va_t ImportSlot);
} // namespace neverd

#endif

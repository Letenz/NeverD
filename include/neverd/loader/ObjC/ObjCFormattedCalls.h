#ifndef NEVERD_LOADER_OBJC_OBJCFORMATTEDCALLS_H
#define NEVERD_LOADER_OBJC_OBJCFORMATTEDCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {
struct BinaryImage;

/// Promoted scalar arguments of a bounded NSString format. Positional slots
/// must be contiguous and consistent; malformed or unsupported forms fail.
std::optional<std::vector<TypeRef>>
objcFormatArgumentTypes(llvm::ArrayRef<uint16_t> Format);

/// Complete a declared message or C call using the same NSString format
/// contract. The caller supplies the independently validated fixed signature.
std::optional<SourceCallTypeHint>
bindObjCFormatArguments(const BinaryImage &Image, SourceCallTypeHint Call,
                        unsigned FormatParameter, va_t FormatAddress);

/// Bind a dynamic message using an immutable format object and a declaration
/// agreed by the SDK and every matching runtime/protocol method.
std::optional<SourceCallTypeHint>
objcFormattedSourceCallHint(const BinaryImage &Image, llvm::StringRef Selector,
                            va_t FormatAddress);
} // namespace neverd
#endif

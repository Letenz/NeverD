#ifndef NEVERD_LOADER_SWIFT_SWIFTVIRTUALCALLS_H
#define NEVERD_LOADER_SWIFT_SWIFTVIRTUALCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Bind only arm64 Swift CGFloat, Double, or Bool getters with an exact
/// masked-isa virtual target, declared Objective-C thunk, and preserved Swift
/// self carrier.
std::map<va_t, SourceCallTypeHint>
buildSwiftVirtualCallHints(const BinaryImage &Image, const LowFunc &Function);

/// Recheck the immutable declaration and import evidence carried by a bound
/// virtual call. The LowIR target and context paths are proved at binding.
bool isSwiftVirtualSourceCallHint(const BinaryImage &Image,
                                  const SourceCallTypeHint &Hint);
} // namespace neverd

#endif

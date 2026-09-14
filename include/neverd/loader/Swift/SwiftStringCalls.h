#ifndef NEVERD_LOADER_SWIFT_SWIFTSTRINGCALLS_H
#define NEVERD_LOADER_SWIFT_SWIFTSTRINGCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Bind only the known, nonthrowing String-to-NSString Swift ABI entry.
/// Arbitrary Swift functions and multi-register results remain unsupported.
std::optional<SourceCallTypeHint>
swiftStringSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

} // namespace neverd
#endif

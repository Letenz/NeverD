#ifndef NEVERD_LOADER_SWIFT_SWIFTSTRINGCALLS_H
#define NEVERD_LOADER_SWIFT_SWIFTSTRINGCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;

/// Bind the fixed Darwin String/NSString bridge entries. Other Swift calls
/// require separate calling-convention and source-type evidence.
std::optional<SourceCallTypeHint>
swiftStringSourceCallHint(const BinaryImage &Image, va_t ImportSlot);

struct SwiftLiteralString {
  static constexpr uint64_t StorageBias = 32;
  static constexpr uint64_t ImmortalTag = UINT64_C(0x8000000000000000);
  va_t Contents = 0;
  uint32_t Bytes = 0;
};

/// Validate a large immortal String literal at its exact Foundation bridge
/// consumer. The returned immutable extent includes its trailing zero. No
/// native heap header, foreign object, or managed storage is inferred.
std::optional<SwiftLiteralString> swiftLiteralString(const BinaryImage &Image,
                                                     va_t ImportSlot,
                                                     uint64_t CountAndFlags,
                                                     uint64_t Storage);

} // namespace neverd
#endif

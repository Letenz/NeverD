#ifndef NEVERD_LOADER_MACHO_IMMUTABLENATIVECALLS_H
#define NEVERD_LOADER_MACHO_IMMUTABLENATIVECALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

struct ImmutableNativeCallTarget {
  va_t FunctionEntry = 0;
  SourceCallOccurrenceKey Site;
  va_t Slot = 0;
  va_t Target = 0;
  bool operator==(const ImmutableNativeCallTarget &) const = default;
};

/// Prove original ARM64 BLR targets loaded from exact immutable chained code
/// pointer slots. The bounded same-block trace validates the instructions that
/// construct the address and the full-width load. Crossing a direct call needs
/// an authenticated runtime declaration or an explicit native callee ABI and
/// a preserved register. Frame reloads and unknown calls remain unsupported.
/// This is target identity only; it supplies no ABI or source publication gate.
std::map<va_t, ImmutableNativeCallTarget> immutableNativeCallTargets(
    const BinaryImage &Image, const LowFunc &Function,
    const std::map<va_t, SourceFunctionTypeHint> *NativeCallees = nullptr);
} // namespace neverd
#endif

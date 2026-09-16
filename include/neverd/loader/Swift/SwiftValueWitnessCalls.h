#ifndef NEVERD_LOADER_SWIFT_SWIFTVALUEWITNESSCALLS_H
#define NEVERD_LOADER_SWIFT_SWIFTVALUEWITNESSCALLS_H

#include "neverd/ir/SourceCallTypeHint.h"

#include <map>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Prove indirect calls through supported required Swift value-witness slots.
/// The proof is structural and symbol-independent: the target must be loaded
/// from metadata[-1][slot], and the same metadata value must occupy that
/// operation's declared Swift argument carrier at the call.
std::map<va_t, SourceCallTypeHint>
buildSwiftValueWitnessCallHints(const BinaryImage &Image,
                                const LowFunc &Function);

} // namespace neverd
#endif

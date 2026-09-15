#ifndef NEVERD_LOADER_READONLYBYTES_H
#define NEVERD_LOADER_READONLYBYTES_H

#include "neverd/Common.h"

#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;

/// Read one uniquely mapped immutable byte range with no pointer fixups.
/// The caller separately proves that copying its contents preserves the use;
/// this routine establishes neither pointer identity nor ownership/lifetime.
std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size);

/// Whether scalar bits could name a source-owned image object or instruction.
/// A pointer requires the target width and a mapped section owner; segment
/// padding and narrow integer pieces do not provide that identity. This check
/// does not authorize byte copies: immutability, fixups and uses are separate.
bool isImagePointerBitPattern(const BinaryImage &Image, uint64_t Bits,
                              uint16_t Width);

/// Read a full-width resolved data pointer from uniquely mapped immutable
/// storage. The relocation must identify the current target's owning range.
/// This proves the loaded value only, not a binding for the slot's address or
/// permission to copy the target object.
std::optional<va_t> readImmutableImagePointer(const BinaryImage &Image,
                                              va_t Address);
} // namespace neverd
#endif

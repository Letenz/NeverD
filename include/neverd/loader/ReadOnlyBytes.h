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
} // namespace neverd
#endif

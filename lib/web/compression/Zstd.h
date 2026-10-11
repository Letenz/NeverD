//===- Zstd.h - Bounded native Zstandard decoder interface --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode into caller-owned output without tools or format policy.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace neverd::web::compression {
bool zstdAvailable();
/// The owning format reader must preflight the frame and bound output/window
/// sizes. Failure may modify Output, which the caller must then discard.
/// A successful count still requires format-specific size/content checks.
std::optional<size_t> decompressZstd(std::string_view Input,
                                     std::span<char> Output);
} // namespace neverd::web::compression

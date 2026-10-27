//===- BunSourceMap.h - Bun serialized source maps ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bun serialized source maps.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Bun.h"
#include "neverd/web/Limits.h"
#include "neverd/web/SourceMap.h"

namespace neverd::web {
inline constexpr std::string_view BunSourceMapProfile =
    "bun-1.4.2-serialized-source-map-v1";

/// Requires an in-process native Zstd library; never invokes a decoder tool.
bool bunSourceMapAvailable();
/// Decode an already extracted module's map, retaining its raw storage links.
/// Malformed structure, unsupported frames and budget failures publish nothing.
SourceMap decodeBunSourceMap(const BunExtraction &Extraction,
                             const BunModule &Module);
} // namespace neverd::web

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::web {
inline constexpr uint64_t MaxBunMapSources =
    ::neverd::limits::web::kBunSourceMapMaxBunMapSources;
inline constexpr uint64_t MaxBunMapSourceBytes =
    ::neverd::limits::web::kBunSourceMapMaxBunMapSourceBytes;
inline constexpr uint64_t MaxBunMapDecodedBytes =
    ::neverd::limits::web::kBunSourceMapMaxBunMapDecodedBytes;
} // namespace neverd::web

//===- DataPack.h - Chromium stored resource evidence ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit, bounded DataPack v5 storage. Numeric IDs grant no filenames,
/// framework identity, runtime routing or source-language claims.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

namespace neverd::web {
inline constexpr std::string_view DataPackProfile = "chromium-datapack-v5-v1";
inline constexpr std::string_view WebViewDataPackProfile =
    "msedge-webview2-datapack-v5-u32-v1";

struct DataPackResource {
  std::string ID, BlobHash;
  Blob Content;
  uint64_t Offset = 0;
  uint32_t CanonicalIndex = 0;
  uint32_t ResourceID = 0;
  bool Alias = false, GzipCandidate = false;
};

struct DataPackExtraction {
  std::string ID, ArtifactID, BlobHash, Profile;
  uint8_t Encoding = 0;
  uint32_t ResourceCount = 0, AliasCount = 0;
  uint64_t IndexBytes = 0, PayloadBytes = 0;
  std::vector<DataPackResource> Resources;
};

struct DataPackDecodedResource {
  std::string ID, ResourceID, ExtractionID, BlobHash;
  Blob Content;
};

/// Explicitly selects standard v5/u16 or the measured WebView2 v5/u32 layout.
/// Accepts its canonical uncompressed header/tables and complete payload.
/// Preserves every original resource, including opaque compressed bytes.
/// Aliases share a physical slice but retain independent occurrence identities.
DataPackExtraction extractDataPack(const Artifact &Input,
                                   std::string_view Profile);
} // namespace neverd::web

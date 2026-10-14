//===- Gzip.h - Bounded native gzip decoding ---------------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A single explicit RFC 1952 stream, with checked trailer and no trailing
/// data.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Blob.h"

namespace neverd::web::compression {
bool gzipAvailable();
Blob decodeGzip(const Blob &Input, uint64_t MaxOutput);
} // namespace neverd::web::compression

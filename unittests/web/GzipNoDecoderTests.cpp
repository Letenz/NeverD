//===- GzipNoDecoderTests.cpp - Honest native codec omission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// No external executable or alternate parser replaces a missing decoder.
///
//===----------------------------------------------------------------------===//

#include "compression/Gzip.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"

TEST(WebGzipWithoutDecoder, ExplicitUnavailableRefusal) {
  EXPECT_FALSE(neverd::web::compression::gzipAvailable());
  try {
    (void)neverd::web::compression::decodeGzip({}, 1);
    FAIL();
  } catch (const neverd::web::Error &E) {
    EXPECT_STREQ(E.what(), "gzip_decoder_unavailable");
  }
}

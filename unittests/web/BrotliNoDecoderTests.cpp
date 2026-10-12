//===- BrotliNoDecoderTests.cpp - Native codec omission contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The same owning codec compiled without its private SDK macro fails clearly.
///
//===----------------------------------------------------------------------===//

#include "compression/Brotli.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/Session.h"
#include "neverd/web/Tauri.h"

TEST(WebBrotliNoDecoder, CapabilityAndExtractionRefuseWithoutExternalFallback) {
  using namespace neverd::web;
  EXPECT_FALSE(compression::brotliAvailable());
  EXPECT_FALSE(tauriAvailable());
  EXPECT_EQ(Session::capabilities().find("tauri_extract"), std::string::npos);
  uint64_t Calls = 0;
  try {
    compression::decodeBrotli("inert", 100, 100, Calls, 10,
                              std::chrono::steady_clock::now());
    FAIL() << "omitted decoder admitted output";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), "brotli_decoder_unavailable");
  }
  try {
    extractTauri({}, {}, TauriProfile);
    FAIL() << "omitted decoder admitted a table";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), "tauri_capability_unavailable");
  }
  EXPECT_EQ(Calls, 0u);
}

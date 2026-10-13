//===- TauriTests.cpp - PHF, mappings and bounded codec guards ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent bytes prove exact decoding, table agreement and atomic refusal.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "TauriFixture.h"
#include "compression/Brotli.h"
#include "desktop/tauri/PHF.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/Tauri.h"

namespace {
using namespace neverd::web;
using namespace tauri_test;
Artifact artifact(std::string_view Bytes) {
  BlobStore Store(Bytes.size());
  Store.append(Bytes);
  Store.seal();
  Artifact A;
  A.ID = "inert-selected-native";
  A.BlobHash = sha256(Bytes);
  A.Content = Store.whole();
  return A;
}
TauriInitializer initializer(const Artifact &A) {
  TauriInitializer I;
  I.InputHash = A.BlobHash;
  I.Start = I.Function = Start;
  I.End = Start + 52;
  I.Offset = 256;
  I.Instructions = 8;
  I.Mappings.push_back({Base, 4096, 0, 4096, true});
  I.Fields = {{{Start + 7, Base + Disps, true},
               {Start + 11, 1, false},
               {Start + 26, Base + Rows, true},
               {Start + 30, 2, false},
               {Start + 48, 0, false}}};
  return I;
}
template <class F> std::string refusal(F Call) {
  try {
    Call();
    ADD_FAILURE() << "atomic refusal required";
  } catch (const Error &E) {
    return E.what();
  }
  return {};
}
} // namespace

TEST(WebTauri, PHFKnownKeysUseSip13_128AndWrappingArithmetic) {
  // rust-siphash 0.3.10 src/tests128.rs's fixed SipHasher13 empty-message
  // vector. Upstream authors/licenses are preserved under LICENSES/phf.
  const auto V =
      tauri_detail::sip13_128({}, 0x0706050403020100ULL, 0x0f0e0d0c0b0a0908ULL);
  EXPECT_EQ(V.Lower, 0xbea58827b2bc7ee7ULL);
  EXPECT_EQ(V.Upper, 0x013030dd6adb62fdULL);
  const auto J = tauri_detail::phfHash("/entry.js", 0);
  const auto H = tauri_detail::phfHash("/index.html", 0);
  EXPECT_EQ(tauri_detail::phfIndex(J, 0, 1, 2), 0u);
  EXPECT_EQ(tauri_detail::phfIndex(H, 0, 1, 2), 1u);
  EXPECT_EQ(tauri_detail::phfIndex({0, UINT32_MAX, UINT32_MAX}, 2, 3, 5), 0u);
}
TEST(WebTauri, CompleteTableRetainsFramesAndDecodedNamespace) {
  if (!tauriAvailable())
    GTEST_SKIP() << "pinned codec/path policy unavailable";
  const auto A = artifact(image());
  const auto E = extractTauri(A, initializer(A), TauriProfile);
  ASSERT_EQ(E.Members.size(), 2u);
  EXPECT_EQ(E.Members[0].Content.read(0, E.Members[0].Content.size()),
            javascript());
  EXPECT_EQ(E.Members[1].Content.read(0, E.Members[1].Content.size()), html());
  EXPECT_EQ(E.Members[0].Stored.read(0, E.Members[0].Stored.size()),
            compressedJS());
  EXPECT_EQ(E.Members[1].BlobHash, sha256(html()));
  EXPECT_EQ(E.DisplacementsOffset, Disps);
  EXPECT_EQ(E.EntriesOffset, Rows);
  EXPECT_EQ(tauriNamespace(E).Artifacts.size(), 3u);
  EXPECT_EQ(E.DecodedBytes, javascript().size() + html().size());
}
TEST(WebTauri, FieldsMappingsLookupsPathsAndIdentitiesRefuse) {
  if (!tauriAvailable())
    GTEST_SKIP() << "pinned codec/path policy unavailable";
  auto A = artifact(image());
  auto I = initializer(A);
  I.Fields[0].ExactAddress = false;
  EXPECT_EQ(refusal([&] { extractTauri(A, I, TauriProfile); }),
            "tauri_invalid_initializer_fields");
  I = initializer(A);
  I.Mappings.push_back(I.Mappings[0]);
  EXPECT_EQ(refusal([&] { extractTauri(A, I, TauriProfile); }),
            "tauri_ambiguous_mapping");
  I = initializer(A);
  I.Fields[2].Value = Base + 4096;
  EXPECT_EQ(refusal([&] { extractTauri(A, I, TauriProfile); }),
            "tauri_non_file_backed_pointer");
  I = initializer(A);
  I.Fields[2].Value = Base + 256;
  EXPECT_EQ(refusal([&] { extractTauri(A, I, TauriProfile); }),
            "tauri_overlapping_evidence_ranges");
  I = initializer(A);
  I.InputHash = std::string(64, '0');
  EXPECT_EQ(refusal([&] { extractTauri(A, I, TauriProfile); }),
            "tauri_input_identity_mismatch");
  auto Bytes = image();
  test::nativePut(Bytes, Disps + 4, 0, 4);
  A = artifact(Bytes);
  EXPECT_EQ(refusal([&] { extractTauri(A, initializer(A), TauriProfile); }),
            "tauri_phf_lookup_mismatch");
  Bytes = image();
  Bytes.replace(640, 9, "/../a.jsx");
  A = artifact(Bytes);
  EXPECT_FALSE(
      refusal([&] { extractTauri(A, initializer(A), TauriProfile); }).empty());
  EXPECT_EQ(refusal([&] { extractTauri(A, initializer(A), "tauri-latest"); }),
            "tauri_unsupported_profile");
}
TEST(WebTauri, CodecAndExtractionBudgetsRefuseWithoutPartialOutput) {
  if (!tauriAvailable())
    GTEST_SKIP() << "pinned codec/path policy unavailable";
  auto Decode = [&](std::string_view B, uint64_t Max,
                    uint64_t Memory = 64ULL * 1024 * 1024,
                    uint64_t CallLimit = 65536) {
    uint64_t Calls = 0;
    return compression::decodeBrotli(B, Max, Memory, Calls, CallLimit,
                                     std::chrono::steady_clock::now() +
                                         std::chrono::seconds(5));
  };
  EXPECT_EQ(Decode(compressedJS(), javascript().size()), javascript());
  EXPECT_EQ(refusal([&] { Decode({}, 4096); }), "brotli_truncated_stream");
  EXPECT_EQ(refusal([&] { Decode(compressedJS(), UINT64_MAX); }),
            "brotli_invalid_limits");
  uint64_t Calls = 0;
  EXPECT_EQ(refusal([&] {
              compression::decodeBrotli(
                  compressedJS(), 4096, 64ULL * 1024 * 1024, Calls, 65536,
                  std::chrono::steady_clock::time_point{});
            }),
            "brotli_time_budget_exceeded");
  EXPECT_EQ(refusal([&] { Decode(compressedJS(), javascript().size() - 1); }),
            "brotli_output_budget_exceeded");
  EXPECT_EQ(refusal([&] { Decode(compressedHTML(), 4096, 1); }),
            "brotli_memory_budget_exceeded");
  EXPECT_EQ(
      refusal([&] { Decode(compressedHTML(), 4096, 64ULL * 1024 * 1024, 0); }),
      "brotli_call_budget_exceeded");
  EXPECT_EQ(refusal([&] { Decode(compressedHTML() + "x", 4096); }),
            "brotli_trailing_bytes");
  EXPECT_EQ(refusal([&] { Decode(compressedHTML().substr(0, 10), 4096); }),
            "brotli_truncated_stream");
  const auto A = artifact(image());
  auto Limits = TauriLimits{};
  Limits.DecodedBytes = 1;
  EXPECT_EQ(
      refusal([&] { extractTauri(A, initializer(A), TauriProfile, Limits); }),
      "brotli_output_budget_exceeded");
}

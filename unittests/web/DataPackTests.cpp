//===- DataPackTests.cpp - Complete resource-table admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original/alias storage, malformed indexes and explicit codec boundaries.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "DataPackFixture.h"
#include "Internal.h"
#include "compression/Gzip.h"
#include "gtest/gtest.h"

#include "neverd/web/DataPack.h"

namespace {
using namespace neverd::web;
using namespace neverd::web::datapack_test;
Artifact input(std::string_view Bytes, std::string_view Occurrence = "one") {
  BlobStore Store(MaxDataPackBytes);
  Store.append(Bytes);
  Store.seal();
  Artifact A;
  A.Content = Store.whole();
  A.BlobHash = sha256(Bytes);
  A.ID = identity("datapack-test-input", {Occurrence, A.BlobHash});
  return A;
}
void refuse(std::string_view Bytes, const char *Code) {
  try {
    (void)extractDataPack(input(Bytes), DataPackProfile);
    FAIL() << Code;
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), Code);
  }
}

TEST(WebDataPack, StoredResourcesAliasesAndEmptyRangesRetainExactOccurrences) {
  const auto Bytes =
      pack({{7, "const privateName=1;"}, {13, ""}, {44, "opaque"}},
           {{9, 0}, {52, 2}});
  const auto E = extractDataPack(input(Bytes), DataPackProfile);
  EXPECT_EQ(E.Encoding, 1);
  EXPECT_EQ(E.ResourceCount, 3U);
  EXPECT_EQ(E.AliasCount, 2U);
  EXPECT_EQ(E.IndexBytes + E.PayloadBytes, Bytes.size());
  ASSERT_EQ(E.Resources.size(), 5U);
  for (const auto &R : E.Resources) {
    EXPECT_EQ(R.Content.read(0, R.Content.size()),
              Bytes.substr(R.Offset, R.Content.size()));
    EXPECT_EQ(R.BlobHash, sha256(Bytes.substr(R.Offset, R.Content.size())));
  }
  EXPECT_TRUE(E.Resources[3].Alias);
  EXPECT_EQ(E.Resources[3].ResourceID, 9);
  EXPECT_EQ(E.Resources[3].Offset, E.Resources[0].Offset);
  EXPECT_EQ(E.Resources[3].BlobHash, E.Resources[0].BlobHash);
  EXPECT_NE(E.Resources[3].ID, E.Resources[0].ID);
  EXPECT_EQ(E.Resources[1].Content.size(), 0U);
  const auto Other = extractDataPack(input(Bytes, "two"), DataPackProfile);
  EXPECT_NE(Other.ID, E.ID);
  EXPECT_NE(Other.Resources[0].ID, E.Resources[0].ID);
  EXPECT_EQ(extractDataPack(input(pack({})), DataPackProfile).Resources.size(),
            0U);
}

TEST(WebDataPack, HeadersAndIncompletePartitionsFailBeforePublishing) {
  const auto Original = pack({{7, "abc"}, {13, "xyz"}}, {{9, 0}});
  for (size_t N = 0; N < 12; ++N)
    refuse(Original.substr(0, N), "datapack_truncated_header");
  auto B = Original;
  number(B, 0, 4, 4);
  refuse(B, "datapack_unsupported_version");
  for (const auto At : {4U, 5U, 6U, 7U}) {
    B = Original;
    number(B, At, 3, 1);
    refuse(B, "datapack_invalid_encoding_or_padding");
  }
  refuse(Original.substr(0, 20), "datapack_truncated_index");
  for (const auto Change : {std::pair{14U, 12U}, std::pair{24U, 1U},
                            std::pair{26U, uint32_t(Original.size() + 1)}}) {
    B = Original;
    number(B, Change.first, Change.second, Change.first == 24 ? 2 : 4);
    refuse(B, "datapack_invalid_payload_partition");
  }
  refuse(Original + "trailer", "datapack_invalid_payload_partition");
  B = Original;
  number(B, 20, 12, 4);
  refuse(B, "datapack_invalid_resource_range");
}

TEST(WebDataPack, DuplicateUnsortedAndOutOfRangeAliasesRefuseAtomically) {
  refuse(pack({{7, "a"}, {7, "b"}}), "datapack_resource_ids_not_unique_sorted");
  refuse(pack({{13, "a"}, {7, "b"}}),
         "datapack_resource_ids_not_unique_sorted");
  refuse(pack({{7, "a"}}, {{7, 0}}), "datapack_alias_ids_not_unique_sorted");
  refuse(pack({{7, "a"}}, {{9, 0}, {9, 0}}),
         "datapack_alias_ids_not_unique_sorted");
  refuse(pack({{7, "a"}}, {{13, 0}, {9, 0}}),
         "datapack_alias_ids_not_unique_sorted");
  refuse(pack({{7, "a"}}, {{9, 1}}), "datapack_alias_index_out_of_range");
  refuse(pack({}, {{9, 0}}), "datapack_alias_index_out_of_range");
  auto A = input(pack({{7, "a"}}));
  A.BlobHash = std::string(64, '0');
  EXPECT_THROW(extractDataPack(A, DataPackProfile), Error);
  EXPECT_THROW(extractDataPack(input(pack({})), "unknown"), Error);
}

TEST(WebDataPack, DeclaredEncodingAndCompressedMagicDoNotRewriteStoredBytes) {
  const auto Compressed = gzipStored("const privateName=42;");
  for (const uint8_t Encoding : {0, 1, 2}) {
    const auto E = extractDataPack(input(pack({{7, Compressed}}, {}, Encoding)),
                                   DataPackProfile);
    EXPECT_EQ(E.Encoding, Encoding);
    ASSERT_EQ(E.Resources.size(), 1U);
    EXPECT_TRUE(E.Resources[0].GzipCandidate);
    EXPECT_EQ(E.Resources[0].Content.read(0, Compressed.size()), Compressed);
  }
}

TEST(WebDataPackGzip, OneCompleteStreamChecksTrailerAndOutputBudget) {
  if (!compression::gzipAvailable())
    GTEST_SKIP() << "Native gzip decoder unavailable";
  const auto Bytes = gzipStored("const privateName=42;");
  const auto Decoded = compression::decodeGzip(input(Bytes).Content, 100);
  EXPECT_EQ(Decoded.read(0, Decoded.size()), "const privateName=42;");
  EXPECT_EQ(Decoded.digest(), sha256("const privateName=42;"));
  EXPECT_THROW(compression::decodeGzip(input(Bytes).Content, 3), Error);
  for (const auto &B : {Bytes.substr(0, Bytes.size() - 1), Bytes + "extra",
                        Bytes + Bytes, std::string("raw")})
    EXPECT_THROW(compression::decodeGzip(input(B).Content, 100), Error);
  auto Corrupt = Bytes;
  Corrupt[Corrupt.size() - 8] ^= 1;
  EXPECT_THROW(compression::decodeGzip(input(Corrupt).Content, 100), Error);
  Corrupt = Bytes;
  Corrupt.back() ^= 1;
  EXPECT_THROW(compression::decodeGzip(input(Corrupt).Content, 100), Error);
}

TEST(WebDataPack,
     ExplicitWideProfilePreservesAll32BitIDsAndRejectsWrongLayout) {
  const auto B =
      widePack({{0, "empty-id"}, {0x12345678, "wide"}, {0xf0000001, "last"}},
               {{0x99999999, 1}});
  const auto E = extractDataPack(input(B), WebViewDataPackProfile);
  EXPECT_EQ(E.Profile, WebViewDataPackProfile);
  ASSERT_EQ(E.Resources.size(), 4U);
  EXPECT_EQ(E.Resources[1].ResourceID, 0x12345678U);
  EXPECT_EQ(E.Resources[2].ResourceID, 0xf0000001U);
  EXPECT_EQ(E.Resources[3].ResourceID, 0x99999999U);
  EXPECT_TRUE(E.Resources[3].Alias);
  EXPECT_EQ(E.Resources[3].CanonicalIndex, 1U);
  EXPECT_EQ(E.Resources[3].BlobHash, E.Resources[1].BlobHash);
  EXPECT_EQ(E.Resources[3].Content.read(0, 4), "wide");
  EXPECT_THROW(extractDataPack(input(B), DataPackProfile), Error);
  EXPECT_THROW(
      extractDataPack(input(pack({{7, "narrow"}})), WebViewDataPackProfile),
      Error);
  EXPECT_THROW(extractDataPack(input(widePack({{7, "a"}}, {{9, UINT32_MAX}})),
                               WebViewDataPackProfile),
               Error);
  EXPECT_THROW(extractDataPack(input(widePack({{7, "a"}}, {{7, 0}})),
                               WebViewDataPackProfile),
               Error);
}

TEST(WebDataPack, WideCountOverflowRefusesBeforeReadingOrAllocatingTables) {
  for (const auto Counts :
       {std::pair{UINT32_MAX, 0U}, std::pair{UINT32_MAX, UINT32_MAX},
        std::pair{65536U, 1U}}) {
    auto B = widePack({});
    number(B, 8, Counts.first, 4);
    number(B, 12, Counts.second, 4);
    try {
      (void)extractDataPack(input(B), WebViewDataPackProfile);
      FAIL();
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), "datapack_resource_budget_exceeded");
    }
  }
}

} // namespace

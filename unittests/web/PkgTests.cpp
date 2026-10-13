//===- PkgTests.cpp - Offline pkg store and envelope contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent byte oracles, hostile layouts and real producer qualification.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "Internal.h"
#include "PkgFixture.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/Pkg.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using namespace neverd::web;
using namespace neverd::web::pkg_test;
std::string readQualification(const std::filesystem::path &Path) {
  std::ifstream File(Path, std::ios::binary | std::ios::ate);
  if (!File || File.tellg() < 0 || uint64_t(File.tellg()) > MaxPkgInputBytes)
    throw std::runtime_error("invalid qualification input");
  const auto Size = size_t(File.tellg());
  File.seekg(0);
  std::string Bytes(Size, '\0');
  if (!File.read(Bytes.data(), Bytes.size()))
    throw std::runtime_error("qualification read failed");
  return Bytes;
}
class WebPkg : public ::testing::Test {
protected:
  void SetUp() override {
    if (!pkgAvailable())
      GTEST_SKIP() << "Pinned portable namespace policy unavailable";
  }
  Artifact input(std::string_view Bytes, std::string_view Occurrence = "one") {
    BlobStore Store(MaxPkgInputBytes);
    Store.append(Bytes);
    Store.seal();
    Artifact A;
    A.Content = Store.whole();
    A.BlobHash = sha256(Bytes);
    A.ID = identity("pkg-test-occurrence", {Occurrence, A.BlobHash});
    return A;
  }
  PkgExtraction extract(std::string_view Bytes) {
    return extractPkg(input(Bytes), PkgProfile);
  }
  void refuse(std::string_view Bytes, const char *Expected) {
    try {
      (void)extract(Bytes);
      FAIL() << Expected;
    } catch (const Error &E) {
      EXPECT_STREQ(E.what(), Expected);
    }
  }
};

TEST_F(WebPkg, BothLocatorStylesAndSnapshotRootsPreserveEveryByte) {
  for (const bool Windows : {false, true}) {
    Builder B;
    const auto Root = Windows ? "C:\\snapshot" : "/snapshot";
    const auto File = Windows ? "C:\\snapshot\\a.js" : "/snapshot/a.js";
    B.directory(Root);
    B.file(File, "abc");
    const auto Bytes = B.build(File, Windows, "{}", "CANARY_TRAILER");
    const auto E = extract(Bytes);
    ASSERT_EQ(E.Regions.size(), 4U);
    uint64_t At = 0;
    for (const auto &R : E.Regions) {
      EXPECT_EQ(R.Offset, At);
      EXPECT_EQ(R.Content.read(0, R.Content.size()),
                Bytes.substr(At, R.Content.size()));
      EXPECT_EQ(R.BlobHash, sha256(Bytes.substr(At, R.Content.size())));
      At += R.Content.size();
    }
    EXPECT_EQ(At, Bytes.size());
    EXPECT_EQ(E.LocatorLineStyle, Windows ? "crlf" : "lf");
    EXPECT_EQ(E.PathStyle, Windows ? "windows_snapshot" : "posix_snapshot");
    EXPECT_EQ(E.PathCount, 2U);
    EXPECT_EQ(E.ContentFileCount, 1U);
    EXPECT_EQ(E.ReferencedBytes, E.PayloadSize);
    EXPECT_EQ(E.HashedBytes, Bytes.size() + B.Payload.size());
    EXPECT_LE(E.ScannedBytes, 2 * Bytes.size());
    for (const auto &M : E.Members)
      if (M.selectable()) {
        EXPECT_EQ(M.Path, "a.js");
        EXPECT_EQ(
            M.BlobHash,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT_EQ(E.LiteralEntryMemberID, M.ID);
        EXPECT_FALSE(M.CachePresent);
      } else {
        EXPECT_TRUE(M.privateHash());
      }
    const auto S = pkgNamespace(E);
    ASSERT_EQ(S.Artifacts.size(), 2U);
    EXPECT_TRUE(S.Artifacts[0].Directory);
    EXPECT_TRUE(S.Artifacts[0].MemberPath.empty());
    EXPECT_EQ(S.Artifacts[1].MemberPath, "a.js");
  }
}

TEST_F(WebPkg,
       CachePresenceDoesNotAuthenticateStoredSourceOrInventMissingBytes) {
  Builder B;
  B.file("/snapshot/both.js", "var source=1;", "INCONSISTENT_OPAQUE_CACHE");
  B.add("/snapshot/cache.js", {{0, "OTHER_CACHE"}, {3, stat(false, 700)}});
  B.add("/snapshot/stat.js", {{3, stat(false, 99)}});
  const auto E = extract(B.build("/snapshot/cache.js"));
  EXPECT_EQ(E.ContentFileCount, 1U);
  EXPECT_EQ(E.CacheOnlyFileCount, 1U);
  EXPECT_EQ(E.UnavailableFileCount, 2U);
  EXPECT_TRUE(E.LiteralEntryMemberID.empty());
  EXPECT_TRUE(E.EntryCachePresent);
  for (const auto &M : E.Members)
    if (M.selectable()) {
      EXPECT_TRUE(M.CachePresent);
      EXPECT_EQ(M.Content.read(0, M.Content.size()), "var source=1;");
    }
  const auto S = pkgNamespace(E);
  ASSERT_EQ(S.Artifacts.size(), 2U);
  EXPECT_EQ(S.Artifacts[1].MemberPath, "both.js");
}

TEST_F(WebPkg, EntryLiteralMatchesRetainAncestorLinkUncertainty) {
  Builder B;
  B.file("/snapshot/alias/main.js", "var n=1;");
  const auto E = extract(B.build("/snapshot/alias/main.js", false,
                                 R"({"/snapshot/alias":"/snapshot/real"})"));
  EXPECT_FALSE(E.LiteralEntryMemberID.empty());
  EXPECT_TRUE(E.EntrySymlinkPrefix);
  ASSERT_EQ(E.Links.size(), 1U);
  EXPECT_EQ(E.Links[0].Target, "real");
  const auto F = extract(B.build("/snapshot/alias/main.js", false,
                                 R"({"/snapshot/alia":"/snapshot/real"})"));
  EXPECT_FALSE(F.EntrySymlinkPrefix);
}

TEST_F(WebPkg, StoredSourceAndOriginalStatSizeAreSeparateEvidence) {
  constexpr std::string_view Stored = "\nmodule.exports=1;\n";
  for (const auto DeclaredSize :
       {Stored.size(), Stored.size() + 19, size_t(1)}) {
    Builder B;
    B.add("/snapshot/main.js",
          {{1, std::string(Stored)}, {3, stat(false, DeclaredSize)}});
    const auto E = extract(B.build("/snapshot/main.js"));
    ASSERT_EQ(E.ContentFileCount, 1U);
    ASSERT_EQ(E.Members.size(), 2U);
    const auto &M = E.Members[0];
    EXPECT_TRUE(M.selectable());
    EXPECT_EQ(M.DeclaredStatSize, DeclaredSize);
    EXPECT_EQ(M.Content.read(0, M.Content.size()), Stored);
    EXPECT_EQ(M.BlobHash, sha256(Stored));
  }
}

TEST_F(WebPkg, LocatorCrossesReadWindowsWithoutDuplicateOrLostCandidates) {
  Builder B;
  B.file("/snapshot/a.js", "x();");
  for (const auto At : {BlobTransferBytes - 512, BlobTransferBytes - 200,
                        BlobTransferBytes - 1, BlobTransferBytes}) {
    const auto E = extract(image(B.Payload, B.code("/snapshot/a.js"), false,
                                 BlobTransferBytes + 2048, At));
    EXPECT_EQ(E.LocatorOffset, At);
    EXPECT_EQ(E.ContentFileCount, 1U);
  }
}

TEST_F(WebPkg, DuplicateLocatorsAndMalformedSlotsRefuse) {
  Builder B;
  B.file("/snapshot/a.js", "x();");
  const auto Good = B.build("/snapshot/a.js");
  const auto E = extract(Good);
  const auto Decl = Good.substr(E.LocatorOffset, E.LocatorSize);
  refuse(Good + Decl, "pkg_ambiguous_locator");
  auto Duplicate = Good;
  Duplicate.replace(512, Decl.size(), Decl);
  refuse(Duplicate, "pkg_ambiguous_locator");
  for (const auto *Bad : {"-1", "+1", "01", "1.5", "1e2", " 1", "2147483648",
                          "NaN", "Infinity"}) {
    auto Bytes = Good;
    const auto At = Bytes.find(" = '") + 4;
    std::string Slot = Bad;
    Slot.resize(22, ' ');
    Bytes.replace(At, 22, Slot);
    refuse(Bytes, "pkg_invalid_locator_size");
  }
  auto Mixed = Good;
  Mixed.insert(Mixed.find(";\n") + 1, 1, '\r');
  refuse(Mixed, "pkg_invalid_locator");
  refuse("no locator", "pkg_locator_not_found");
}

TEST_F(WebPkg, IndexCompressionAndStoreTypesFailClearly) {
  Builder B;
  B.file("/snapshot/a.js", "x();");
  for (const auto *Compression : {"1", "2", "3", "null", "\"0\""})
    refuse(image(B.Payload, prelude(B.VFS + '}', "/snapshot/a.js", "{}", "{}",
                                    Compression)),
           "pkg_compression_unsupported");
  refuse(image(B.Payload,
               prelude(B.VFS + '}', "/snapshot/a.js", "{}", R"({"a":"b"})")),
         "pkg_compression_unsupported");
  refuse(image(B.Payload, ";" + B.code("/snapshot/a.js")),
         "pkg_unsupported_prelude");
  Builder Missing;
  Missing.add("/snapshot/a.js", {{1, "a"}});
  refuse(Missing.build("/snapshot/a.js"), "pkg_missing_stat");
  Builder Conflict;
  Conflict.add("/snapshot/a.js", {{1, "a"}, {3, stat(true)}});
  refuse(Conflict.build("/snapshot/a.js"), "pkg_conflicting_store_kind");
  Builder Unknown;
  Unknown.add("/snapshot/a.js", {{4, "a"}, {3, stat(false, 1)}});
  refuse(Unknown.build("/snapshot/a.js"), "pkg_invalid_store_range");
}

TEST_F(WebPkg, PortablePathRulesAndRootUniquenessAreNotHostNormalization) {
  for (const auto *Path :
       {"/else/a", "/snapshot/../a", "/snapshot/CON", "/snapshot/a//b",
        "/snapshot/a/", "/snapshot/a\\b", "C:\\snapshot\\a/b", "/snapshot/"}) {
    Builder B;
    B.file(Path, "x");
    EXPECT_THROW(extract(B.build(Path)), Error) << Path;
  }
  for (const auto &Names : {std::pair{"/snapshot/a", "/snapshot/A"},
                            std::pair{"/snapshot/a", "/snapshot/a/b"},
                            std::pair{"/snapshot/é", "/snapshot/e\xcc\x81"},
                            std::pair{"/snapshot/a", "C:\\snapshot\\b"}}) {
    Builder B;
    B.file(Names.first, "x");
    B.file(Names.second, "y");
    EXPECT_THROW(extract(B.build(Names.first)), Error);
  }
}

TEST_F(WebPkg, TruncatedFramesAndMalformedIndexRangesNeverPublishMembers) {
  Builder B;
  B.file("/snapshot/a.js", "var n=1;");
  const auto Good = B.build("/snapshot/a.js");
  for (size_t End = 0; End < Good.size(); ++End)
    EXPECT_THROW(extract(std::string_view(Good).substr(0, End)), Error) << End;
  for (const auto *Pair : {"[-1,0]", "[0,-1]", "[0.5,0]", "[0,999999]",
                           "[999999,0]", "[0,0,0]", "[\"0\",0]", "[1e99,0]"}) {
    const auto VFS = "{\"/snapshot/a.js\":{\"3\":" + std::string(Pair) + "}}";
    refuse(image(B.Payload, prelude(VFS, "/snapshot/a.js")),
           "pkg_invalid_store_range");
  }
  for (const auto *Index :
       {R"({"/snapshot/a":{"3":[0,0]},"/snapshot/\u0061":{"3":[0,0]}})",
        R"({"/snapshot/\ud800":{"3":[0,0]}})",
        R"({"/snapshot/a":{"3":[0,0],}})"})
    EXPECT_THROW(extract(image("", prelude(Index, "/snapshot/a"))), Error);
  auto BadUTF8 = B.code("/snapshot/a.js");
  const auto Body = BadUTF8.find("CANARY_PRELUDE_BODY");
  ASSERT_NE(Body, BadUTF8.npos);
  BadUTF8.insert(Body, 1, char(0xff));
  refuse(image(B.Payload, BadUTF8), "pkg_invalid_prelude_utf8");
}

TEST_F(WebPkg, EqualInputBytesInDifferentOccurrencesKeepSeparateMembers) {
  Builder B;
  B.file("/snapshot/a.js", "x();");
  const auto Bytes = B.build("/snapshot/a.js");
  const auto A = extractPkg(input(Bytes, "a"), PkgProfile);
  const auto C = extractPkg(input(Bytes, "b"), PkgProfile);
  EXPECT_NE(A.ID, C.ID);
  for (size_t I = 0; I < A.Members.size(); ++I) {
    EXPECT_EQ(A.Members[I].BlobHash, C.Members[I].BlobHash);
    EXPECT_NE(A.Members[I].ID, C.Members[I].ID);
  }
  auto Bad = input(Bytes);
  Bad.BlobHash = std::string(64, '0');
  EXPECT_THROW(extractPkg(Bad, PkgProfile), Error);
  EXPECT_THROW(extractPkg(Bad, "pkg-latest"), Error);
}

TEST_F(WebPkg, ExactAliasesShareReadsWhilePartialOverlapsRefuse) {
  const auto Stat = stat(false, 3);
  const auto Tail = ",\"3\":[3," + std::to_string(Stat.size()) + "]}";
  const auto VFS = "{\"/snapshot/a.js\":{\"1\":[0,3]" + Tail +
                   ",\"/snapshot/b.js\":{\"1\":[0,3]" + Tail + '}';
  const auto Bytes = image("abc" + Stat, prelude(VFS, "/snapshot/a.js"));
  const auto E = extract(Bytes);
  ASSERT_EQ(E.Members.size(), 4U);
  EXPECT_NE(E.Members[0].ID, E.Members[2].ID);
  EXPECT_EQ(E.Members[0].RangeID, E.Members[2].RangeID);
  EXPECT_EQ(E.Members[0].BlobHash, E.Members[2].BlobHash);
  EXPECT_EQ(E.UniqueStoreRanges, 2U);
  EXPECT_EQ(E.HashedBytes, Bytes.size() + 3 + Stat.size());
  EXPECT_EQ(E.ScannedBytes,
            Bytes.size() + E.Regions[2].Content.size() + Stat.size());
  auto Overlap = VFS;
  Overlap.replace(Overlap.rfind("[0,3]"), 5, "[1,3]");
  refuse(image("abc" + Stat, prelude(Overlap, "/snapshot/a.js")),
         "pkg_partial_store_overlap");
  Builder Empty;
  Empty.file("/snapshot/empty.js", "");
  const auto Z = extract(Empty.build("/snapshot/empty.js"));
  EXPECT_EQ(Z.Members[0].Content.size(), 0U);
  EXPECT_EQ(Z.Members[0].BlobHash,
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(WebPkg, MetadataRangeAliasesKeepTheirDigestPrivateAcrossStoreKinds) {
  auto Stat = stat(false);
  for (unsigned I = 0; I < 3; ++I)
    Stat = stat(false, Stat.size());
  const auto Size = std::to_string(Stat.size());
  const auto Pair = "[0," + Size + "]";
  const auto VFS = "{\"/snapshot/a\":{\"0\":" + Pair + ",\"1\":" + Pair +
                   ",\"3\":" + Pair + "}}";
  const auto E = extract(image(Stat, prelude(VFS, "/snapshot/a")));
  ASSERT_EQ(E.Members.size(), 3U);
  EXPECT_EQ(E.UniqueStoreRanges, 1U);
  for (const auto &M : E.Members) {
    EXPECT_TRUE(M.privateHash());
    EXPECT_TRUE(M.CachePresent);
    EXPECT_FALSE(M.selectable());
  }
  EXPECT_TRUE(E.LiteralEntryMemberID.empty());
  EXPECT_TRUE(E.EntryCachePresent);
  EXPECT_EQ(pkgNamespace(E).Artifacts.size(), 1U);
  EXPECT_TRUE(E.Regions[1].MetadataRange);
}

TEST_F(WebPkg, IndependentPreludeIndexPathAndStatBudgetsRefuse) {
  refuse(image("", std::string(MaxPkgPreludeBytes + 1, 'x')),
         "pkg_prelude_budget_exceeded");
  refuse(
      image("", prelude(std::string(MaxPkgIndexBytes + 1, ' '), "/snapshot/a")),
      "pkg_index_budget_exceeded");
  const auto Stat = stat(false);
  std::string Many = "{";
  for (uint32_t I = 0; I <= MaxPkgPaths; ++I) {
    if (I)
      Many += ',';
    Many += quote("/snapshot/f" + std::to_string(I)) + ":{\"3\":[0," +
            std::to_string(Stat.size()) + "]}";
  }
  refuse(image(Stat, prelude(Many + '}', "/snapshot/f0")),
         "pkg_path_budget_exceeded");
  Builder BigStat;
  BigStat.add("/snapshot/a", {{3, std::string(4097, ' ')}});
  refuse(BigStat.build("/snapshot/a"), "pkg_stat_budget_exceeded");
}

TEST_F(WebPkg, ComparisonAndPrivateMetadataBudgetsAreIndependent) {
  const auto Stat = stat(false);
  auto Build = [&](std::string_view Prefix, uint32_t Count) {
    std::string VFS = "{";
    for (uint32_t I = 0; I < Count; ++I) {
      if (I)
        VFS += ',';
      VFS += quote(std::string(Prefix) + std::to_string(I)) + ":{\"3\":[0," +
             std::to_string(Stat.size()) + "]}";
    }
    return image(Stat, prelude(VFS + '}', std::string(Prefix) + '0'));
  };
  refuse(Build("/snapshot/" + std::string(180, 'a'), 4000),
         "pkg_work_budget_exceeded");
  std::string Deep = "/snapshot/";
  for (unsigned I = 0; I < 60; ++I)
    Deep += "segment/";
  refuse(Build(Deep, 300), "pkg_metadata_budget_exceeded");
}

TEST_F(WebPkg, AggregateJSONAndLinkCountLimitsApplyAcrossPrivateDocuments) {
  auto Stat = stat(false);
  Stat.pop_back();
  Stat += ",\"extra\":[0";
  for (unsigned I = 0; I < 1500; ++I)
    Stat += ",0";
  Stat += "]}";
  ASSERT_LT(Stat.size(), 4096U);
  Builder B;
  for (unsigned I = 0; I < 68; ++I)
    B.add("/snapshot/" + std::to_string(I), {{3, Stat}});
  refuse(B.build("/snapshot/0"), "json_node_budget_exceeded");

  Builder One;
  One.file("/snapshot/a.js", "x();");
  std::string Links = "{";
  for (unsigned I = 0; I <= MaxPkgLinks; ++I) {
    if (I)
      Links += ',';
    Links += quote("/snapshot/link" + std::to_string(I)) + ":" +
             quote("/snapshot/a.js");
  }
  refuse(One.build("/snapshot/a.js", false, Links + '}'),
         "pkg_link_budget_exceeded");
}

TEST_F(WebPkg, OptionalSixPlatformProducerImagesMatchIndependentInputs) {
  const char *Root = std::getenv("NEVERD_PKG_581_CORPUS");
  if (!Root || !*Root)
    GTEST_SKIP() << "Pinned pkg producer corpus not supplied";
  const std::filesystem::path Directory(Root);
  const std::vector<std::pair<std::string, std::string>> Images{
      {"linux-arm64",
       "d8f7b3cbe4a001a76b7f7faafab1b6dd5a478806d8ab35e9561b47011839be93"},
      {"linux-x64",
       "9dcbf9544be6a0b6914f7cc6a070c04adb473e4d208a3b1041729c69088cde37"},
      {"macos-arm64",
       "9587d836227ad298d8e3b3d0fea7700dceeb64f6aabcebab41822b578c583524"},
      {"macos-x64",
       "48ba3079f42eab5606348641bc5231be1a9e92741ba4ae8da1fcf70d74550341"},
      {"win-arm64.exe",
       "d083f6e8c3fe1a3fb1d61134139e4d0ee75501ab5b517dbbd00175650ab1b227"},
      {"win-x64.exe",
       "8284b0997b6308f08208c97e83b21a647d7747ed1a5ff5eed24086244f3b1598"}};
  const std::vector<std::pair<std::string, std::string>> Inputs{
      {"main.js",
       "e83f5d03da6abb2dca1d100990ea8ced5dab99f3da07bfbbad21cd08fc5e6fbf"},
      {"minified.js",
       "cb6cb1717cadfd48094b0057039efe776dd3b3d40785fa1c1003da6a9ac35438"},
      {"asset.txt",
       "c1af5b987a6467f5f44787b5887b2287f0cf0112cd71187d61b39117c3db913d"},
      {"package.json",
       "ee11bdd9884192e81c1b16836af767e68fbb08a4019cc98cd5ad83f83b907526"}};
  for (const auto &[Name, Hash] : Images) {
    SCOPED_TRACE(Name);
    const auto Bytes = readQualification(Directory / "outputs-canonical" /
                                         ("neverd-pkg-fixture-" + Name));
    ASSERT_EQ(sha256(Bytes), Hash);
    const auto E = extract(Bytes);
    EXPECT_EQ(E.ContentFileCount, 4U);
    EXPECT_EQ(E.DirectoryCount, 2U);
    EXPECT_EQ(E.Members.size(), 12U);
    EXPECT_EQ(E.PayloadSize, 1189U);
    EXPECT_EQ(E.PayloadSize, E.ReferencedBytes);
    EXPECT_FALSE(E.LiteralEntryMemberID.empty());
    EXPECT_FALSE(E.EntrySymlinkPrefix);
    EXPECT_EQ(E.Regions.back().Content.size() > 0, Name.starts_with("macos"));
    EXPECT_EQ(E.LocatorLineStyle, Name.starts_with("win") ? "crlf" : "lf");
    for (const auto &[InputName, InputHash] : Inputs) {
      const auto Original = readQualification(Directory / "inputs" / InputName);
      ASSERT_EQ(sha256(Original), InputHash);
      bool Found = false;
      for (const auto &M : E.Members)
        if (M.selectable() && M.Path == "inputs/" + InputName) {
          Found = true;
          EXPECT_EQ(M.BlobHash, InputHash);
          EXPECT_EQ(M.Content.read(0, M.Content.size()), Original);
          EXPECT_FALSE(M.CachePresent);
        }
      EXPECT_TRUE(Found) << InputName;
    }
  }
}

TEST_F(WebPkg,
       OptionalProducerPreprocessingPreservesStoredAndOriginalEvidence) {
  const char *Root = std::getenv("NEVERD_PKG_581_CORPUS");
  if (!Root || !*Root)
    GTEST_SKIP() << "Pinned pkg producer corpus not supplied";
  const std::filesystem::path Directory(Root);
  const auto Bytes = readQualification(Directory / "outputs-stripped" /
                                       "neverd-pkg-stripped-linux-x64");
  ASSERT_EQ(sha256(Bytes),
            "6109fc614eea8d88e74981994add4b16376f4e071accf21cefe3b5b52e28d7bb");
  const auto E = extract(Bytes);
  ASSERT_EQ(E.ContentFileCount, 4U);
  EXPECT_EQ(E.PayloadSize, 1048U);
  struct Input {
    std::string_view Name, Original, Stored, OriginalHash, StoredHash;
  };
  constexpr Input Inputs[] = {
      {"main.js",
       "\xef\xbb\xbf#!/usr/bin/env node\n'use strict';\n"
       "module.exports=require('./minified.js');\n",
       "\n'use strict';\nmodule.exports=require('./minified.js');\n",
       "95d9088e01c2d6e5f4be7d35f5acecc986a9d0c5185c31cb005d9d8859633c4d",
       "6c82717398c6489401a3266008c7f452851a0374bfb2a836cb910307ec5a897a"},
      {"minified.js", "\xef\xbb\xbfmodule.exports=42;\n",
       "module.exports=42;\n",
       "cc650a9fb6d092eab83bee757b7240e1e3c063c155428994e13fcfc7e1145f45",
       "d8ba574f0d3f0f0f6efee8cb42cd4f197a83fc6e4bc8a76a1bc69f409cdf4a57"}};
  for (const auto &I : Inputs) {
    SCOPED_TRACE(I.Name);
    const auto Original =
        readQualification(Directory / "stripped-inputs" / I.Name);
    ASSERT_EQ(Original, I.Original);
    ASSERT_EQ(sha256(Original), I.OriginalHash);
    ASSERT_EQ(sha256(I.Stored), I.StoredHash);
    bool Found = false;
    for (const auto &M : E.Members)
      if (M.selectable() &&
          M.Path == "stripped-inputs/" + std::string(I.Name)) {
        Found = true;
        EXPECT_EQ(M.Content.read(0, M.Content.size()), I.Stored);
        EXPECT_EQ(M.BlobHash, I.StoredHash);
        EXPECT_EQ(M.DeclaredStatSize, Original.size());
        EXPECT_LT(M.Content.size(), M.DeclaredStatSize);
        EXPECT_FALSE(M.CachePresent);
      }
    EXPECT_TRUE(Found);
  }
}
} // namespace

//===- BunSourceMapCorpusTests.cpp - Maps in real six-target containers --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pinned producer images must recover the independently recorded input bytes.
///
//===----------------------------------------------------------------------===//

#include "BunSourceMap.h"
#include "Internal.h"
#include "ProducerCorpus.h"
#include "gtest/gtest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using namespace neverd::web;

TEST(WebBunSourceMapCorpus,
     OptionalSixPlatformImagesRecoverIndependentOriginalSources) {
#ifdef _WIN32
  ASSERT_TRUE(test::producerPrerequisite(false, "POSIX input capture"));
  GTEST_SKIP() << "POSIX capture required";
#endif
  ASSERT_TRUE(
      test::producerPrerequisite(bunSourceMapAvailable(), "native Zstd"));
  if (!bunSourceMapAvailable())
    GTEST_SKIP() << "Native Zstd decoding is unavailable";
  const auto *Linux = std::getenv("NEVERD_BUN_142_CORPUS");
  const auto *Cross = std::getenv("NEVERD_BUN_142_CROSS_CORPUS");
  ASSERT_TRUE(test::producerCorpus("NEVERD_BUN_142_CORPUS", Linux));
  ASSERT_TRUE(test::producerCorpus("NEVERD_BUN_142_CROSS_CORPUS", Cross));
  if (!Linux || !*Linux || !Cross || !*Cross)
    GTEST_SKIP() << "Both pinned Bun 1.4.2 image corpora are required";
  struct Target {
    const char *Name, *Format, *Platform, *Architecture;
  };
  for (const auto &T : {
           Target{"linux-x64", "elf", "linux", "x64"},
           Target{"linux-arm64", "elf", "linux", "arm64"},
           Target{"macos-x64", "macho", "macos", "x64"},
           Target{"macos-arm64", "macho", "macos", "arm64"},
           Target{"windows-x64", "pe", "windows", "x64"},
           Target{"windows-arm64", "pe", "windows", "arm64"},
       }) {
    const bool First = std::string_view(T.Name) == "linux-x64";
    for (const std::string Variant : {"asset-map", "cache-map"}) {
      const auto Name = First ? Variant : std::string(T.Name) + "-" + Variant;
      SCOPED_TRACE(std::string(T.Name) + "/" + Variant);
      const auto ManifestPath = std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) /
                                "bun" / (First ? "" : "cross") /
                                (Name + ".manifest.json");
      std::ifstream File(ManifestPath, std::ios::binary);
      ASSERT_TRUE(File.good());
      const std::string Text(std::istreambuf_iterator<char>(File), {});
      auto Manifest = llvm::json::parse(Text);
      if (!Manifest)
        FAIL() << llvm::toString(Manifest.takeError());
      const auto *Object = Manifest->getAsObject();
      ASSERT_NE(Object, nullptr);
      const auto Path = std::filesystem::path(First ? Linux : Cross) /
                        (Name + (First                                ? ".elf"
                                 : std::string_view(T.Format) == "pe" ? ".exe"
                                                                      : ""));
      const auto Snapshot = capture(Path.string(), Limits{});
      ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
      const auto &A = Snapshot.Artifacts.front();
      const auto ExpectedHash = Object->getString(
          First ? "full_elf_sha256" : "full_container_sha256");
      ASSERT_TRUE(ExpectedHash);
      ASSERT_EQ(A.BlobHash, ExpectedHash->str());
      const auto E = extractBun(A);
      EXPECT_EQ(E.ContainerFormat, T.Format);
      EXPECT_EQ(E.Platform, T.Platform);
      EXPECT_EQ(E.Architecture, T.Architecture);
      ASSERT_FALSE(E.Modules.empty());
      const auto &M = E.Modules.front();
      const auto Map = decodeBunSourceMap(E, M);
      ASSERT_EQ(Map.Sources.size(), 1u);
      ASSERT_TRUE(Map.Sources.front().Contents);
      const auto &Original = *Map.Sources.front().Contents;
      // These hashes come from the self-authored input files in the corpus
      // recipe, independently of NeverD's map/container readers.
      EXPECT_EQ(sha256(Original), Variant == "asset-map"
                                      ? "ada8ba82b03b7bd85876cc812e0108150d3046"
                                        "5e0945ac4b0798ecdb7db31433"
                                      : "dae4f14862158ace4ba0cfb51115157698a528"
                                        "6192f7883ff89123a17b4ad687");
      ASSERT_FALSE(Map.Segments.empty());
      const TextCoordinates SourceCoordinates(Original);
      const auto Generated = bunSourceBytes(E, M, MaxBunMapSourceBytes);
      const TextCoordinates GeneratedCoordinates(Generated);
      for (const auto &Segment : Map.Segments) {
        EXPECT_EQ(Segment.SourceIndex, 0u);
        EXPECT_NO_THROW(SourceCoordinates.byteOffset(Segment.Original));
        EXPECT_NO_THROW(GeneratedCoordinates.byteOffset(Segment.Generated));
      }
      ASSERT_TRUE(Map.Sources.front().Storage);
      const auto &Storage = *Map.Sources.front().Storage;
      const auto &Region = E.Regions.at(M.SourceMap);
      EXPECT_EQ(Map.ArtifactID, Region.ID);
      EXPECT_EQ(Storage.ContentHash, sha256(Region.Content.read(
                                         Storage.ContentOffset - Region.Offset,
                                         Storage.ContentSize)));
    }
  }
}
} // namespace

//===- ClientFeatureCorpusTests.cpp - Pinned client syntax coverage -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sequential source-only coverage over an optional retained native package.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/SourceFeatures.h"
#include "neverd/web/SourceNavigation.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <map>

namespace {
using namespace neverd::web;

TEST(WebSourceFeatures, CapturedClaudeClientUnitsWhenSupplied) {
#ifdef _WIN32
  GTEST_SKIP() << "POSIX capture host qualification required";
#endif
  const auto *Directory = std::getenv("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
  if (!Directory)
    GTEST_SKIP() << "Optional captured fixed-version platform package absent";
  if (!packageArchiveAvailable() || !packageGzipAvailable())
    GTEST_SKIP() << "Native gzip and the archive path policy are required";
  const auto S = capture(
      (std::filesystem::path(Directory) / "darwin-arm64" / "input").string(),
      Limits{});
  ASSERT_EQ(S.Artifacts.size(), 4);
  ASSERT_EQ(S.Artifacts[1].BlobHash,
            "50fbf594d6526c9aae7d66b8b45a08d89ae6b863e2bc9777bfb277d6521a7458");
  const auto Archive = extractPackageArchive(S.Artifacts[1], "tgz");
  ASSERT_EQ(Archive.Members.size(), 4);
  const auto Namespace = packageArchiveNamespace(Archive);
  const auto Original = std::find_if(
      Namespace.Artifacts.begin(), Namespace.Artifacts.end(),
      [&](const Artifact &A) { return A.ID == Archive.Members[0].ID; });
  ASSERT_NE(Original, Namespace.Artifacts.end());
  ASSERT_EQ(Original->BlobHash,
            "c9b5341637becbd423ddffc5b254afb645682a3868cb708bbc6cc0e7bb419937");
  const auto E = extractBun(*Original);
  ASSERT_EQ(E.Profile, "bun-71d0d439-prelinked-macos-arm64-macho-v1");
  uint64_t Sources = 0, Bytes = 0, Parsed = 0, ParserUnavailable = 0;
  uint64_t SourceByteRefusals = 0, ParserWorkRefusals = 0,
           ParserTimeRefusals = 0;
  uint64_t FeatureUnits = 0, FeatureUnavailable = 0, Conditions = 0,
           Callbacks = 0;
  std::array<uint64_t, 5> Counts{}, Unresolved{};
  std::map<std::string, uint64_t> ParserReasons;
  for (const auto &M : E.Modules) {
    if (M.SourceArtifactID.empty())
      continue;
    const auto Text = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
    ++Sources;
    Bytes += Text.size();
    SourceAnalysis Source;
    try {
      Source = inspectJavaScript(M.SourceArtifactID, Text, "module");
    } catch (const Error &Failure) {
      const std::string_view Code(Failure.what());
      if (Code == "source_byte_budget_exceeded")
        ++SourceByteRefusals;
      else if (Code == "parser_budget_exceeded")
        ++ParserWorkRefusals;
      else if (Code == "parser_deadline_exceeded")
        ++ParserTimeRefusals;
      else
        throw;
      ++ParserUnavailable;
      ++ParserReasons[std::string(Code)];
      continue;
    }
    if (Source.ParseStatus != "parsed") {
      ++ParserUnavailable;
      for (const auto &D : Source.Diagnostics)
        ++ParserReasons[D.Code];
      continue;
    }
    ++Parsed;
    const auto B = analyzeSourceBindings(Source);
    const auto N = analyzeSourceNavigation(Source, B);
    const auto A = analyzeClientSourceFeatures(Source, B, N);
    ASSERT_EQ(A.SourceID, Source.ID);
    ASSERT_EQ(A.NavigationID, N.ID);
    if (A.Status != "partial") {
      ++FeatureUnavailable;
      continue;
    }
    ++FeatureUnits;
    for (const auto &R : A.Records) {
      ++Counts[unsigned(R.Domain)];
      Unresolved[unsigned(R.Domain)] += R.Status == "unresolved_shape";
      Conditions += R.Condition != NoSourceIndex;
      Callbacks += R.CallbackFunction != NoSourceIndex;
    }
  }
  EXPECT_EQ(Sources, 2364);
  EXPECT_EQ(Bytes, 45035899);
  EXPECT_EQ(Parsed + ParserUnavailable, Sources);
  EXPECT_EQ(FeatureUnits + FeatureUnavailable, Parsed);
  EXPECT_GT(FeatureUnits, 0);
  RecordProperty("source_count", std::to_string(Sources));
  RecordProperty("source_bytes", std::to_string(Bytes));
  RecordProperty("parsed_units", std::to_string(Parsed));
  RecordProperty("parser_unavailable_units", std::to_string(ParserUnavailable));
  RecordProperty("source_byte_budget_refusals",
                 std::to_string(SourceByteRefusals));
  RecordProperty("parser_work_budget_refusals",
                 std::to_string(ParserWorkRefusals));
  RecordProperty("parser_deadline_refusals",
                 std::to_string(ParserTimeRefusals));
  RecordProperty("feature_inspected_units", std::to_string(FeatureUnits));
  RecordProperty("feature_unavailable_units",
                 std::to_string(FeatureUnavailable));
  constexpr const char *Names[] = {"tool", "configuration", "permission", "mcp",
                                   "hook"};
  for (unsigned I = 0; I < Counts.size(); ++I) {
    RecordProperty(std::string(Names[I]) + "_candidates",
                   std::to_string(Counts[I]));
    RecordProperty(std::string(Names[I]) + "_unresolved_shapes",
                   std::to_string(Unresolved[I]));
  }
  RecordProperty("permission_test_containments", std::to_string(Conditions));
  RecordProperty("callback_source_links", std::to_string(Callbacks));
  for (const auto &[Reason, Count] : ParserReasons)
    RecordProperty("parser_reason_" + Reason, std::to_string(Count));
}

} // namespace

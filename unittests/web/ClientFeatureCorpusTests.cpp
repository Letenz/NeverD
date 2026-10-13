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
#include "neverd/web/ConfigurationFiles.h"
#include "neverd/web/ConfigurationOrder.h"
#include "neverd/web/Error.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/SourceFeatures.h"
#include "neverd/web/SourceLimits.h"
#include "neverd/web/SourceNavigation.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <map>

namespace {
using namespace neverd::web;

void inspectCapturedClientUnits(SourceAnalysisMode Mode) {
  const auto RecordProperty = [](const std::string &Key,
                                 const std::string &Value) {
    ::testing::Test::RecordProperty(Key, Value);
  };
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
  uint64_t OrderUnits = 0, OrderUnavailable = 0, OrderObjects = 0,
           OrderTerms = 0, OrderOverwrites = 0, OrderLoadingLinks = 0;
  uint64_t FileUnits = 0, FileUnavailable = 0, FileSites = 0, FileCalls = 0,
           FileSequences = 0, FileTerms = 0;
  uint64_t FirstUnknownOrderModule = UINT64_MAX;
  std::string FirstUnknownOrderHash;
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
      Source = Mode == SourceAnalysisMode::Large
                   ? inspectLargeJavaScript(M.SourceArtifactID, Text, "module")
                   : inspectJavaScript(M.SourceArtifactID, Text, "module");
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
    const auto Files = analyzeConfigurationFiles(Source, B, N);
    if (Files.Status == "partial") {
      ++FileUnits;
      FileSites += Files.Sites.size();
      FileCalls += Files.Calls.size();
      FileSequences += Files.Sequences.size();
      FileTerms += Files.Terms;
    } else {
      ++FileUnavailable;
    }
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
    const auto Order = analyzeConfigurationSourceOrder(Source, B, N, A);
    if (Order.Status != "partial") {
      ++OrderUnavailable;
      continue;
    }
    ++OrderUnits;
    OrderObjects += Order.Objects.size();
    OrderTerms += Order.Terms;
    for (const auto &O : Order.Objects) {
      for (const auto &T : O.Terms)
        OrderLoadingLinks += T.Feature != NoSourceIndex;
      for (const auto &Field : O.Fields) {
        if (!Field.Seen || !Field.LaterUnknown)
          continue;
        ++OrderOverwrites;
        if (FirstUnknownOrderModule == UINT64_MAX) {
          FirstUnknownOrderModule = &M - E.Modules.data();
          FirstUnknownOrderHash = Source.BlobHash;
        }
      }
    }
  }
  EXPECT_EQ(Sources, 2364);
  EXPECT_EQ(Bytes, 45035899);
  EXPECT_EQ(Parsed + ParserUnavailable, Sources);
  EXPECT_EQ(FeatureUnits + FeatureUnavailable, Parsed);
  EXPECT_GT(FeatureUnits, 0);
  EXPECT_EQ(OrderUnits + OrderUnavailable, FeatureUnits);
  if (Mode == SourceAnalysisMode::Large) {
    EXPECT_EQ(Parsed, Sources);
    EXPECT_EQ(FeatureUnavailable, 0u);
    EXPECT_EQ(OrderUnavailable, 0u);
  }
  RecordProperty("parser_profile", std::string(sourceLimits(Mode).Profile));
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
  RecordProperty("configuration_order_units", std::to_string(OrderUnits));
  RecordProperty("configuration_order_unavailable_units",
                 std::to_string(OrderUnavailable));
  RecordProperty("configuration_order_objects", std::to_string(OrderObjects));
  RecordProperty("configuration_order_terms", std::to_string(OrderTerms));
  RecordProperty("configuration_order_later_unknown_fields",
                 std::to_string(OrderOverwrites));
  RecordProperty("configuration_order_loading_syntax_links",
                 std::to_string(OrderLoadingLinks));
  EXPECT_EQ(FileUnits, Parsed);
  EXPECT_EQ(FileUnavailable, 0);
  EXPECT_GT(FileSites, 0);
  EXPECT_GT(FileCalls, 0);
  EXPECT_GT(FileSequences, 0);
  RecordProperty("configuration_file_units", std::to_string(FileUnits));
  RecordProperty("configuration_file_unavailable_units",
                 std::to_string(FileUnavailable));
  RecordProperty("configuration_file_literal_sites", std::to_string(FileSites));
  RecordProperty("configuration_file_lexical_calls", std::to_string(FileCalls));
  RecordProperty("configuration_file_layer_sequences",
                 std::to_string(FileSequences));
  RecordProperty("configuration_file_layer_terms", std::to_string(FileTerms));
  if (FirstUnknownOrderModule != UINT64_MAX) {
    RecordProperty("configuration_first_unknown_module",
                   std::to_string(FirstUnknownOrderModule));
    RecordProperty("configuration_first_unknown_source_sha256",
                   FirstUnknownOrderHash);
  }
  for (const auto &[Reason, Count] : ParserReasons)
    RecordProperty("parser_reason_" + Reason, std::to_string(Count));
}

TEST(WebSourceFeatures, CapturedClaudeClientUnitsWhenSupplied) {
  inspectCapturedClientUnits(SourceAnalysisMode::Interactive);
}

TEST(WebSourceFeatures, CapturedClaudeLargeClientUnitsWhenSupplied) {
  inspectCapturedClientUnits(SourceAnalysisMode::Large);
}

} // namespace

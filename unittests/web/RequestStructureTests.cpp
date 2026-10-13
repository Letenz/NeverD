//===- RequestStructureTests.cpp - Request component syntax witnesses ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent template, header and body anchors with conservative limits.
///
//===----------------------------------------------------------------------===//

#include "../../lib/web/interfaces/report/Publication.h"
#include "Internal.h"
#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/PassiveReport.h"

#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <set>

using namespace neverd::web;
namespace {
struct Projected {
  SourceAnalysis Source;
  PassiveSourceEvidence Evidence;
  explicit Projected(std::string_view Code)
      : Source(inspectJavaScript("a", Code, "module")) {
    const auto B = analyzeSourceBindings(Source);
    const auto N = analyzeSourceNavigation(Source, B);
    const auto F = analyzeClientSourceFeatures(Source, B, N);
    const auto I =
        analyzeSourceInterfaces(Source, B, analyzeSourceValues(Source));
    const auto R = analyzeInterfaceSourceRelations(Source, B, I, N);
    Evidence = projectPassiveSource(Source, B, N, F, I, R);
  }
};
} // namespace

TEST(WebRequestStructure, TemplateComponentsHeadersAndBodyKeepExactAnchors) {
  const std::string Code =
      R"(fetch(`https://CANARY.invalid/users/${id}?page=${page}`, {
    method:"POST",
    headers:{"Content-Type":"application/json","X-CANARY":requestId},
    body:JSON.stringify({count:1,flag:true})
  });)";
  const Projected P(Code);
  ASSERT_EQ(P.Source.ParseStatus, "parsed");
  std::set<std::string> Nodes;
  uint64_t Expressions = 0, Segments = 0, Headers = 0, BodyFields = 0;
  bool QueryName = false, PathName = false;
  for (const auto &R : P.Evidence.Records) {
    if (!R.Structure)
      continue;
    const auto &S = *R.Structure;
    EXPECT_TRUE(Nodes.insert(S.NodeID).second);
    EXPECT_EQ(R.EvidenceClass, "static_inference");
    EXPECT_TRUE(R.CoordinatesPresent);
    ASSERT_LE(R.Offset + R.Length, Code.size());
    if (S.Edge == "template_expression") {
      const auto Text = Code.substr(R.Offset, R.Length);
      if (Text == "id") {
        EXPECT_EQ(S.Component, "path_segment");
        EXPECT_EQ(S.ComponentIndex, 2u);
      } else {
        EXPECT_EQ(Text, "page");
        EXPECT_EQ(S.Component, "query_value");
        EXPECT_EQ(S.ComponentIndex, 0u);
      }
      ++Expressions;
    }
    Segments += S.Edge == "template_segment";
    if (S.Edge == "lexical_component") {
      ASSERT_TRUE(S.DecodedOffset);
      ASSERT_TRUE(S.DecodedLength);
      for (const auto &N : P.Source.Nodes) {
        if (N.ID != R.RelatedID)
          continue;
        ASSERT_NE(N.text("cooked"), nullptr);
        const auto Text =
            N.text("cooked")->substr(*S.DecodedOffset, *S.DecodedLength);
        QueryName |= S.Component == "query_name" && Text == u"page";
        PathName |= S.Component == "path_segment" && Text == u"users";
      }
    }
    if (R.Domain == "request_headers" && S.Edge == "object_field") {
      ++Headers;
      EXPECT_TRUE(S.NameClass == "content-type" || S.NameClass == "other");
    }
    if (R.Domain == "json_stringify_input" && S.Edge == "object_field") {
      ++BodyFields;
      EXPECT_TRUE(R.ShapeKind == "number" || R.ShapeKind == "boolean");
      EXPECT_EQ(R.Reason, "literal_syntax");
    }
    std::string Text;
    llvm::raw_string_ostream(Text)
        << llvm::json::Value(passive_report::record(R));
    EXPECT_EQ(Text.find("CANARY"), std::string::npos);
    EXPECT_EQ(Text.find("requestId"), std::string::npos);
  }
  EXPECT_EQ(Expressions, 2u);
  EXPECT_EQ(Segments, 3u);
  EXPECT_EQ(Headers, 2u);
  EXPECT_EQ(BodyFields, 2u);
  EXPECT_TRUE(QueryName);
  EXPECT_TRUE(PathName);
  for (const auto &R : P.Evidence.Records)
    if (R.Structure && !R.Structure->ParentNodeID.empty())
      EXPECT_TRUE(Nodes.count(R.Structure->ParentNodeID));
}

TEST(WebRequestStructure, UnsupportedDispatchAndOptionsDoNotInventRoles) {
  const Projected P(R"(api.fetch(url,{headers:{CANARY:1},body:{CANARY:1}});
    fetch(...args);
    fetch(url,{...opts,headers:{CANARY:1},body:{CANARY:1}});
    const opts={headers:{CANARY:1},body:{CANARY:1}}; fetch(url,opts);)");
  for (const auto &R : P.Evidence.Records)
    if (R.Structure)
      EXPECT_EQ(R.Domain, "request_url");
}

TEST(WebRequestStructure,
     GettersSpreadsAndDeferredBodiesStayExplicitlyUnknown) {
  const Projected P(R"(fetch(url, {
    headers:{get CANARY(){return {hidden:1}},...extra,["computed"]:1},
    body:()=>JSON.stringify({hidden:1})
  });)");
  uint64_t UnknownProperties = 0, DeferredRoots = 0;
  for (const auto &R : P.Evidence.Records) {
    if (!R.Structure)
      continue;
    if (R.Domain == "request_headers" &&
        R.Structure->Edge == "unresolved_property") {
      ++UnknownProperties;
      EXPECT_EQ(R.ShapeKind, "unknown");
    }
    if (R.Domain == "request_body") {
      ++DeferredRoots;
      EXPECT_EQ(R.Structure->Edge, "root");
      EXPECT_EQ(R.ShapeKind, "unknown");
    }
    EXPECT_NE(R.Domain, "json_stringify_input");
  }
  EXPECT_EQ(UnknownProperties, 3u);
  EXPECT_EQ(DeferredRoots, 1u);
}

TEST(WebRequestStructure, InterpolationCannotResolveLaterComponents) {
  const Projected P(
      "fetch(`https://CANARY.invalid/${path}?q=${value}#CANARY`);");
  bool Later = false;
  for (const auto &R : P.Evidence.Records) {
    if (R.Kind == "source_interface")
      EXPECT_EQ(R.Reason, "unresolved_template");
    if (R.Structure && R.Structure->Component == "query_name") {
      EXPECT_EQ(R.Reason, "lexical_component_after_unresolved_interpolation");
      Later = true;
    }
  }
  EXPECT_TRUE(Later);
}

TEST(WebRequestStructure, StructuralProjectionChargesTheSharedRecordBudget) {
  std::string Code;
  for (unsigned I = 0; I != 1500; ++I)
    Code += "fetch('https://CANARY.invalid/a/b?q=1');";
  EXPECT_THROW(Projected{Code}, Error);
}

TEST(WebRequestStructure, TemplateExpansionNeverCreatesHTTPMatches) {
  const auto S = inspectJavaScript(
      "a", "fetch(`https://example.invalid/users/${id}`);", "module");
  const auto B = analyzeSourceBindings(S);
  const auto I = analyzeSourceInterfaces(S, B, analyzeSourceValues(S));
  const auto N = analyzeSourceNavigation(S, B);
  const auto F = analyzeClientSourceFeatures(S, B, N);
  const auto Links = analyzeInterfaceSourceRelations(S, B, I, N);
  const auto Source = projectPassiveSource(S, B, N, F, I, Links);
  const auto HAR = inspectHAR("h", R"({"log":{"version":"1.2","entries":[{
    "request":{"method":"GET","url":"https://example.invalid/users/1"}}]}})");
  EXPECT_EQ(composePassiveReport(&Source, &I, &HAR, {}, nullptr).HTTPPairs, 0u);
}

TEST(WebRequestStructure, EscapedComponentsUseCookedUnitsAndContainingSpans) {
  const std::string Code =
      R"(fetch("https://CANARY.invalid/\u00e9?q=\u{1F331}");)";
  const Projected P(Code);
  bool Path = false, Query = false;
  for (const auto &R : P.Evidence.Records) {
    if (!R.Structure || R.Structure->Edge != "lexical_component")
      continue;
    const auto &S = *R.Structure;
    EXPECT_EQ(Code.substr(R.Offset, R.Length),
              R"("https://CANARY.invalid/\u00e9?q=\u{1F331}")");
    if (S.Component == "path_segment") {
      Path = true;
      EXPECT_EQ(S.DecodedLength, 1u);
    } else if (S.Component == "query_value") {
      Query = true;
      EXPECT_EQ(S.DecodedLength, 2u);
    }
  }
  EXPECT_TRUE(Path);
  EXPECT_TRUE(Query);
}

TEST(WebRequestStructure,
     CapturedClaudeTemplatesKeepOriginalAnchorsWhenSupplied) {
  const auto *Base = std::getenv("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
  if (!Base)
    GTEST_SKIP() << "Optional NeverD-exported pinned platform sources absent";
  struct Case {
    const char *File, *Hash;
    uint64_t Size, Records, TemplateOffset, TemplateLength;
    std::array<uint64_t, 2> Offsets, Lengths;
  };
  const Case Cases[] = {
      {"m01357.js",
       "d0e0757751dec0b768fcd07d5430eb8d3ea785fbdb135ed021fdacbe255e9dc8",
       15187,
       10,
       6286,
       27,
       {6306, 6310},
       {1, 1}},
      {"m01740.js",
       "c3ac807c5931807f85455223fccc717deac2148575bc8de9e73e039a6ec4ae8b",
       87530,
       8,
       86342,
       20,
       {86345, 86349},
       {1, 3}}};
  for (const auto &Input : Cases) {
    SCOPED_TRACE(Input.File);
    const auto Path =
        std::filesystem::path(Base) / "darwin-arm64" / "recovered" / Input.File;
    const auto Snapshot = capture(Path.string(), Limits{});
    ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
    const auto &Raw = Snapshot.Artifacts.front();
    ASSERT_EQ(Raw.BlobHash, Input.Hash);
    ASSERT_EQ(Raw.Content.size(), Input.Size);
    const auto Bytes = Raw.Content.read(0, Input.Size, MaxJavaScriptBytes);
    const Projected P(Bytes);
    ASSERT_EQ(P.Source.ParseStatus, "parsed");
    uint64_t Records = 0, Expressions = 0, Segments = 0;
    for (const auto &R : P.Evidence.Records) {
      if (!R.Structure)
        continue;
      ++Records;
      const auto &S = *R.Structure;
      EXPECT_EQ(R.EvidenceClass, "static_inference");
      if (R.Domain == "request_url" && S.Edge == "root") {
        EXPECT_EQ(R.Offset, Input.TemplateOffset);
        EXPECT_EQ(R.Length, Input.TemplateLength);
        EXPECT_EQ(R.Reason, "unresolved_template");
        EXPECT_EQ(Bytes[R.Offset], '`');
        EXPECT_EQ(Bytes[R.Offset + R.Length - 1], '`');
      }
      if (S.Edge == "template_expression") {
        ASSERT_LT(Expressions, 2u);
        EXPECT_EQ(R.Offset, Input.Offsets[Expressions]);
        EXPECT_EQ(R.Length, Input.Lengths[Expressions]);
        EXPECT_EQ(Bytes.substr(R.Offset - 2, 2), "${");
        EXPECT_EQ(Bytes[R.Offset + R.Length], '}');
        EXPECT_EQ(R.ShapeKind, "unknown");
        ++Expressions;
      }
      Segments += S.Edge == "template_segment";
      // Both real calls pass unresolved header/body variables. Do not invent
      // their fields merely because the URL template is now decomposed.
      EXPECT_NE(S.Edge, "object_field");
    }
    EXPECT_EQ(Records, Input.Records);
    EXPECT_EQ(Expressions, 2u);
    EXPECT_EQ(Segments, 3u);
  }
}

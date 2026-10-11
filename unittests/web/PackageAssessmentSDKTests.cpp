//===- PackageAssessmentSDKTests.cpp - Selected evidence and review APIs
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic selection, session-wide review budgets, redaction and native CLI.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using Value = llvm::json::Value;
using Object = llvm::json::Object;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), Text.npos);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid response");
  }
  return std::move(*V);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string encode(Value V) { return llvm::formatv("{0}", V).str(); }
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}
const llvm::json::Array &items(const Value &V) {
  return *V.getAsObject()->getArray("items");
}
const Value *kind(const Value &Page, llvm::StringRef Kind) {
  for (const auto &F : items(Page))
    if (field(F, "kind") == Kind)
      return &F;
  return nullptr;
}

class WebPackageAssessmentSDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  std::string Revision, Packages;
  void write(std::string_view Name, std::string_view Text) {
    std::ofstream Out(Root / std::string(Name), std::ios::binary);
    Out << Text;
  }
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-assessment-sdk", Dir));
    Root = Dir.str().str();
    write("a.package.json", R"({"name":"CANARY","version":"1.0.0",
      "scripts":{"install":"node g.source.js CANARY_ARGUMENT"}})");
    write("b.registry.json", R"({"name":"CANARY","version":"1.0.0","dist":{
      "integrity":"sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=",
      "tarball":"CANARY_URL","signatures":[{"keyid":"CANARY_KEY","sig":"YWJj"}]}})");
    write("c.descriptor.json", R"({"schema_version":1,"source":"CANARY_SOURCE",
      "retrieved_at":"2026-10-11T12:00:00Z","as_of":"2026-10-11T11:00:00Z"})");
    write("d.original", "abc");
    write("e.advisory.json", R"({"id":"CANARY_PRIVATE_ID",
      "modified":"2026-10-10T00:00:00Z", "affected":[{"package":{
      "ecosystem":"npm","name":"CANARY"},"versions":["1.0.0"]}]})");
    write("f.statement.json", R"({"_type":"https://in-toto.io/Statement/v1",
      "subject":[{"name":"CANARY_SUBJECT","digest":{"sha256":
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}}],
      "predicateType":"CANARY_UNSUPPORTED_PREDICATE"})");
    write("g.source.js", R"(const cp = require('node:child_process');
      cp.exec('CANARY_ARGUMENT');)");
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open();
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    if (Root.empty())
      return;
    std::error_code EC;
    std::filesystem::remove(Root.string() + ".stdout", EC);
    std::filesystem::remove(Root.string() + ".stderr", EC);
    std::filesystem::remove_all(Root, EC);
  }
  std::string artifact(uint64_t Index) {
    const auto Page = take(neverd_web_artifacts_json(
        S, Revision.data(), Revision.size(), Index, 1));
    return items(Page).size() == 1 ? field(items(Page)[0], "artifact_id") : "";
  }
  void open() {
    const auto Path = Root.string();
    const auto Preview = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    const auto C =
        take(neverd_web_import_commit_json(S, Token.data(), Token.size()));
    ASSERT_EQ(field(C, "status"), "ok");
    Revision = field(C, "revision");
    const auto Input = artifact(1);
    const auto P = take(neverd_web_packages_analyze_json(
        S, Revision.data(), Revision.size(), Input.data(), Input.size(),
        "package-json", 12));
    ASSERT_EQ(field(P, "status"), "ok");
    Packages = field(P, "package_analysis_id");
  }
  Value create(Object Options = Object{{"schema_version", 1}}) {
    const auto Text = encode(Value(std::move(Options)));
    return createText(Text);
  }
  Value createText(const std::string &Options) {
    return take(neverd_web_package_assessment_create_json(
        S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
        Options.data(), Options.size()));
  }
  Value page(const Value &A, uint64_t Offset = 0, uint64_t Limit = 512,
             const std::string &HeadOverride = {}) {
    const auto ID = field(A, "package_assessment_id");
    const auto Head =
        HeadOverride.empty() ? field(A, "review_head_id") : HeadOverride;
    return take(neverd_web_package_assessment_findings_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Head.data(),
        Head.size(), Offset, Limit));
  }
  Value report(const Value &A, uint64_t Offset = 0, uint64_t Limit = 512) {
    const auto ID = field(A, "package_assessment_id"),
               Head = field(A, "review_head_id");
    return take(neverd_web_package_assessment_report_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Head.data(),
        Head.size(), Offset, Limit));
  }
  Value decisions(const Value &A) {
    const auto ID = field(A, "package_assessment_id"),
               Head = field(A, "review_head_id");
    return take(neverd_web_package_review_decisions_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Head.data(),
        Head.size(), 0, 512));
  }
  Value append(const Value &A, const std::string &Finding,
               const char *Disposition = "needs_review") {
    const auto ID = field(A, "package_assessment_id"),
               Head = field(A, "review_head_id");
    return take(neverd_web_package_review_append_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Head.data(),
        Head.size(), Finding.data(), Finding.size(), Disposition,
        std::strlen(Disposition)));
  }
  Value attestation(uint64_t Index, const char *Kind = "registry-dist") {
    const auto Input = artifact(Index), Descriptor = artifact(3);
    return take(neverd_web_package_attestation_import_json(
        S, Revision.data(), Revision.size(), Input.data(), Input.size(),
        Descriptor.data(), Descriptor.size(), Kind, std::strlen(Kind)));
  }
  Object selected() {
    const auto T = attestation(2);
    const auto Input = artifact(2), Original = artifact(4);
    const auto I = take(neverd_web_package_integrity_verify_json(
        S, Revision.data(), Revision.size(), Original.data(), Original.size(),
        Input.data(), Input.size(), nullptr, 0));
    const auto AdvisoryInput = artifact(5), Descriptor = artifact(3);
    const auto D = take(neverd_web_package_advisory_import_json(
        S, Revision.data(), Revision.size(), AdvisoryInput.data(),
        AdvisoryInput.size(), Descriptor.data(), Descriptor.size()));
    const auto Selection = encode(Value(Object{
        {"schema_version", 1},
        {"advisory_ids", llvm::json::Array{field(D, "package_advisory_id")}}}));
    const auto M = take(neverd_web_package_advisories_match_json(
        S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
        Selection.data(), Selection.size()));
    EXPECT_EQ(field(I, "integrity_status"), "match");
    EXPECT_EQ(field(M, "status"), "ok");
    return Object{
        {"schema_version", 1},
        {"integrity_ids", llvm::json::Array{field(I, "integrity_id")}},
        {"attestation_ids",
         llvm::json::Array{field(T, "package_attestation_id")}},
        {"advisory_matches_id", field(M, "package_advisory_matches_id")}};
  }
  Value metadata() { return take(neverd_web_metadata_json(S)); }
};

TEST_F(WebPackageAssessmentSDK, ExplicitCoverageAndTypedSelectionsStayPrivate) {
  const auto Empty = create();
  ASSERT_EQ(field(Empty, "status"), "ok");
  EXPECT_EQ(Empty.getAsObject()->getInteger("finding_count"), 10);
  const auto Bare = page(Empty);
  ASSERT_EQ(items(Bare).size(), 10u);
  for (const auto &F : items(Bare)) {
    EXPECT_EQ(field(F, "disposition"), "unreviewed");
    EXPECT_EQ(field(F, "disposition_source"), "not_reviewed");
  }
  const auto A = create(selected());
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_NE(field(A, "package_assessment_id"),
            field(Empty, "package_assessment_id"));
  EXPECT_EQ(A.getAsObject()->getBoolean("installed_tree_verified"), false);
  EXPECT_EQ(A.getAsObject()->getBoolean("publisher_authenticated"), false);
  EXPECT_EQ(field(A, "safety_verdict"), "not_assessed");
  const auto P = page(A);
  const auto *Integrity = kind(P, "integrity");
  ASSERT_NE(Integrity, nullptr);
  EXPECT_EQ(field(*Integrity, "evidence_status"), "match");
  EXPECT_EQ(field(*Integrity, "binding"),
            "declared_identity_original_inventory_unbound");
  bool Match = false;
  for (const auto &F : items(P))
    Match |= field(F, "kind") == "advisory" &&
             field(F, "evidence_status") == "declared_version_match";
  EXPECT_TRUE(Match);
  const auto R = field(report(A), "report_markdown");
  EXPECT_NE(R.find("2026-10-11T12:00:00Z"), R.npos);
  EXPECT_EQ(R.find("YWJj"), R.npos);
  EXPECT_EQ(R.find("ungWv48B"), R.npos);
  EXPECT_NE(R.find("no safe-package conclusion"), R.npos);
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_assessment_count"),
            2);
}

TEST_F(WebPackageAssessmentSDK,
       SubjectDigestSelectionRetainsOnlyOriginalBinding) {
  const auto T = attestation(6, "intoto-statement");
  const auto ID = field(T, "package_attestation_id");
  const auto P = take(neverd_web_package_attestation_records_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), "digests", 7,
      0, 512));
  ASSERT_EQ(items(P).size(), 1u);
  const auto Digest = field(items(P).front(), "digest_id"),
             Original = artifact(4);
  const auto M = take(neverd_web_package_attestation_digest_match_json(
      S, Revision.data(), Revision.size(), Original.data(), Original.size(),
      ID.data(), ID.size(), Digest.data(), Digest.size()));
  ASSERT_EQ(field(M, "integrity_status"), "match");
  const auto A = create(Object{
      {"schema_version", 1},
      {"attestation_ids", llvm::json::Array{ID}},
      {"attestation_digest_match_ids",
       llvm::json::Array{field(M, "package_attestation_digest_match_id")}}});
  ASSERT_EQ(field(A, "status"), "ok");
  const auto Findings = page(A);
  const Value *DigestFinding = nullptr;
  for (const auto &F : items(Findings))
    if (field(F, "binding") == "selected_subject_original_only")
      DigestFinding = &F;
  ASSERT_NE(DigestFinding, nullptr);
  EXPECT_EQ(field(*DigestFinding, "kind"), "attestation");
  EXPECT_EQ(field(*DigestFinding, "artifact_id"), Original);
  EXPECT_TRUE(field(*DigestFinding, "package_id").empty());
  auto Wrong = create(
      Object{{"schema_version", 1},
             {"attestation_ids", llvm::json::Array{ID}},
             {"integrity_ids", llvm::json::Array{field(M, "integrity_id")}}});
  EXPECT_EQ(code(Wrong), "unknown_package_integrity");
}

TEST_F(WebPackageAssessmentSDK, ReviewChangesPreserveEvidenceAndLastReplay) {
  const auto A = create();
  const auto P0 = page(A);
  const auto Finding = field(items(P0)[1], "finding_id");
  const auto U1 = append(A, Finding);
  ASSERT_EQ(field(U1, "status"), "ok");
  EXPECT_EQ(U1.getAsObject()->getBoolean("replayed"), false);
  const auto Replay = append(A, Finding);
  EXPECT_EQ(Replay.getAsObject()->getBoolean("replayed"), true);
  EXPECT_EQ(field(Replay, "review_head_id"), field(U1, "review_head_id"));
  EXPECT_EQ(code(append(A, Finding, "false_positive")),
            "stale_package_review_head");
  EXPECT_EQ(code(page(A)), "stale_package_review_head");
  EXPECT_EQ(code(report(A)), "stale_package_review_head");
  EXPECT_EQ(code(decisions(A)), "stale_package_review_head");
  const auto U2 = append(U1, Finding, "false_positive");
  ASSERT_EQ(field(U2, "status"), "ok");
  EXPECT_EQ(code(append(A, Finding)), "stale_package_review_head");
  const auto P2 = page(U2);
  EXPECT_EQ(items(P0).size(), items(P2).size());
  for (size_t I = 0; I < items(P0).size(); ++I) {
    const auto &Before = items(P0)[I], &After = items(P2)[I];
    for (const auto *Key : {"finding_id", "evidence_status", "binding",
                            "confidence", "owner_id", "record_id"})
      EXPECT_EQ(field(Before, Key), field(After, Key));
  }
  EXPECT_EQ(field(items(P2)[1], "disposition"), "false_positive");
  EXPECT_EQ(field(items(P2)[1], "disposition_source"), "caller_assertion");
  const auto History = decisions(U2);
  ASSERT_EQ(items(History).size(), 2u);
  EXPECT_EQ(field(items(History)[0], "disposition"), "needs_review");
  EXPECT_EQ(field(items(History)[1], "parent_head_id"),
            field(U1, "review_head_id"));
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_review_event_count"),
            2);
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_review_retained_bytes"),
      2048);
  EXPECT_NE(field(report(U2), "report_markdown").find("false_positive"),
            std::string::npos);
  // Recreating the same evidence recipe returns the current head, not a reset.
  EXPECT_EQ(field(create(), "review_head_id"), field(U2, "review_head_id"));
}

TEST_F(WebPackageAssessmentSDK,
       RefusalsAreAtomicAndCaptureResetRevokesReviews) {
  const auto A = create();
  const auto Before = encode(metadata());
  for (const auto *Text :
       {R"({"schema_version":2})", R"({"schema_version":1,"raw":true})",
        R"({"schema_version":1,"notes":"CANARY"})"})
    EXPECT_EQ(field(createText(Text), "status"), "error");
  EXPECT_EQ(code(createText(R"({"schema_version":1,"integrity_ids":["x"]})")),
            "package_metadata_invalid_identity");
  EXPECT_EQ(code(create(Object{{"schema_version", 1},
                               {"package_behavior_id", std::string(64, '0')}})),
            "unknown_package_behavior");
  EXPECT_EQ(code(page(A, 0, 0)), "invalid_page");
  EXPECT_EQ(code(page(A, 0, 513)), "invalid_page");
  EXPECT_EQ(code(page(A, 11)), "invalid_page");
  EXPECT_EQ(code(append(A, std::string(64, '0'))),
            "unknown_package_assessment_finding");
  EXPECT_EQ(code(append(A, field(items(page(A))[0], "finding_id"), "safe")),
            "invalid_package_disposition");
  EXPECT_EQ(encode(metadata()), Before);
  const auto OldRevision = Revision;
  open();
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_assessment_count"),
            0);
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_review_event_count"),
            0);
  EXPECT_EQ(code(page(A)), "unknown_package_assessment");
  const auto Current = Revision;
  Revision = OldRevision;
  EXPECT_EQ(code(page(A)), "stale_revision");
  Revision = Current;
}

TEST_F(WebPackageAssessmentSDK, AssessmentCeilingDoesNotPublishAThirdRecipe) {
  const auto A = create();
  ASSERT_EQ(field(A, "status"), "ok");
  auto Selection = selected();
  ASSERT_EQ(field(create(Object(Selection)), "status"), "ok");
  Selection.erase("integrity_ids");
  const auto Before = encode(metadata());
  EXPECT_EQ(code(create(std::move(Selection))),
            "package_assessment_cache_budget_exceeded");
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(field(create(), "package_assessment_id"),
            field(A, "package_assessment_id"));
}

TEST_F(WebPackageAssessmentSDK, InventoryFindingsPageAcrossTheRecordBoundary) {
  Object Entries;
  Entries[""] = Object{{"name", "CANARY"}, {"version", "1.0.0"}};
  for (unsigned I = 0; I < 600; ++I)
    Entries["node_modules/CANARY_" + std::to_string(I)] =
        Object{{"name", "CANARY_" + std::to_string(I)}, {"version", "1.0.0"}};
  write("a.package.json",
        encode(Value(
            Object{{"lockfileVersion", 3}, {"packages", std::move(Entries)}})));
  open();
  const auto Input = artifact(1);
  const auto P = take(neverd_web_packages_analyze_json(
      S, Revision.data(), Revision.size(), Input.data(), Input.size(),
      "npm-lock", 8));
  ASSERT_EQ(field(P, "status"), "ok");
  Packages = field(P, "package_analysis_id");
  const auto A = create();
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_EQ(A.getAsObject()->getInteger("finding_count"), 610);
  const auto First = page(A);
  EXPECT_EQ(items(First).size(), 512u);
  EXPECT_EQ(First.getAsObject()->getInteger("next_offset"), 512);
  const auto Last = page(A, 512);
  EXPECT_EQ(items(Last).size(), 98u);
  EXPECT_EQ(Last.getAsObject()->getBoolean("page_complete"), true);
  EXPECT_EQ(items(page(A, 610)).size(), 0u);
  EXPECT_EQ(code(page(A, 611)), "invalid_page");
  EXPECT_EQ(report(A).getAsObject()->getInteger("next_offset"), 512);
  EXPECT_EQ(report(A, 512).getAsObject()->getBoolean("page_complete"), true);
}

TEST_F(WebPackageAssessmentSDK, ReviewEventBudgetIsSharedByBothAssessments) {
  auto A = create();
  auto B = create(selected());
  const auto FA = field(items(page(A))[0], "finding_id"),
             FB = field(items(page(B))[0], "finding_id");
  for (unsigned I = 0; I < 256; ++I) {
    auto &Current = I % 2 ? A : B;
    Current = append(Current, I % 2 ? FA : FB);
    ASSERT_EQ(field(Current, "status"), "ok");
  }
  EXPECT_EQ(A.getAsObject()->getInteger("decision_count"), 128);
  EXPECT_EQ(B.getAsObject()->getInteger("decision_count"), 128);
  const auto Before = encode(metadata());
  EXPECT_EQ(code(append(A, FA)), "package_review_decision_budget_exceeded");
  EXPECT_EQ(code(append(B, FB)), "package_review_decision_budget_exceeded");
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_review_event_count"),
            256);
  EXPECT_EQ(items(decisions(A)).size(), 128u);
  EXPECT_EQ(items(decisions(B)).size(), 128u);
}

TEST_F(WebPackageAssessmentSDK, SelectedSourcePatternsKeepByteAndNodeAnchors) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Selected source patterns require the optional JS frontend";
#else
  const auto Input = artifact(7);
  const auto Source = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), Input.data(), Input.size(),
      "commonjs", 8));
  const auto SourceID = field(Source, "source_id");
  const auto Options = encode(Value(Object{
      {"schema_version", 1}, {"source_ids", llvm::json::Array{SourceID}}}));
  const auto B = take(neverd_web_package_behavior_analyze_json(
      S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
      Options.data(), Options.size()));
  ASSERT_EQ(field(B, "status"), "ok");
  const auto BehaviorID = field(B, "package_behavior_id");
  const auto InstallOptions = encode(Value(
      Object{{"schema_version", 1}, {"package_behavior_id", BehaviorID}}));
  const auto I = take(neverd_web_package_install_analyze_json(
      S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
      InstallOptions.data(), InstallOptions.size()));
  ASSERT_EQ(field(I, "status"), "ok");
  const auto A =
      create(Object{{"schema_version", 1},
                    {"package_behavior_id", BehaviorID},
                    {"package_install_id", field(I, "package_install_id")}});
  ASSERT_EQ(field(A, "status"), "ok");
  const auto P = page(A);
  bool Pattern = false, Association = false;
  for (const auto &F : items(P)) {
    if (field(F, "kind") == "behavior" && !field(F, "node_id").empty()) {
      Pattern = true;
      EXPECT_EQ(field(F, "artifact_id"), Input);
      EXPECT_EQ(field(F, "source_id"), SourceID);
      EXPECT_EQ(field(F, "api_class"), "child_process.exec");
      EXPECT_FALSE(field(F, "byte_offset").empty());
      EXPECT_FALSE(field(F, "byte_length").empty());
    }
    Association |=
        field(F, "kind") == "installation" && !field(F, "node_id").empty();
  }
  EXPECT_TRUE(Pattern);
  EXPECT_TRUE(Association);
#endif
}

TEST_F(WebPackageAssessmentSDK,
       SelectedComparisonKeepsCountsAndOccurrenceOwners) {
#if !NEVERD_TEST_WEB_JAVASCRIPT
  GTEST_SKIP() << "Selected comparison requires the optional JS frontend";
#else
  write("h.package.json", R"({"name":"CANARY","version":"1.0.1"})");
  open();
  const auto Input = artifact(8);
  const auto After = take(neverd_web_packages_analyze_json(
      S, Revision.data(), Revision.size(), Input.data(), Input.size(),
      "package-json", 12));
  const auto AfterID = field(After, "package_analysis_id");
  const auto SourceInput = artifact(7);
  const auto Source = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), SourceInput.data(),
      SourceInput.size(), "commonjs", 8));
  const auto SourceID = field(Source, "source_id");
  const auto Selection = encode(Value(Object{
      {"schema_version", 1}, {"source_ids", llvm::json::Array{SourceID}}}));
  const auto Behavior = [&](const std::string &P) {
    return take(neverd_web_package_behavior_analyze_json(
        S, Revision.data(), Revision.size(), P.data(), P.size(),
        Selection.data(), Selection.size()));
  };
  const auto BeforeB = Behavior(Packages), AfterB = Behavior(AfterID);
  const auto BeforeBID = field(BeforeB, "package_behavior_id"),
             AfterBID = field(AfterB, "package_behavior_id");
  const auto D = take(neverd_web_packages_compare_json(
      S, Revision.data(), Revision.size(), Packages.data(), Packages.size(),
      AfterID.data(), AfterID.size()));
  ASSERT_EQ(field(D, "status"), "ok");
  const auto DiffID = field(D, "package_diff_id");
  const auto Options =
      encode(Value(Object{{"schema_version", 1},
                          {"before_package_behavior_id", BeforeBID},
                          {"after_package_behavior_id", AfterBID}}));
  const auto BD = take(neverd_web_package_behavior_diff_analyze_json(
      S, Revision.data(), Revision.size(), DiffID.data(), DiffID.size(),
      Options.data(), Options.size()));
  ASSERT_EQ(field(BD, "status"), "ok");
  const auto A = create(Object{
      {"schema_version", 1},
      {"package_behavior_id", BeforeBID},
      {"package_diff_id", DiffID},
      {"package_behavior_diff_id", field(BD, "package_behavior_diff_id")}});
  ASSERT_EQ(field(A, "status"), "ok");
  const auto Findings = page(A);
  bool Counts = false;
  unsigned Occurrences = 0;
  for (const auto &F : items(Findings)) {
    if (field(F, "evidence_status") == "selected_recognized_pattern_counts") {
      Counts = true;
      EXPECT_EQ(field(F, "api_class"), "child_process.exec");
      EXPECT_EQ(F.getAsObject()->getInteger("recognized_count_before"), 1);
      EXPECT_EQ(F.getAsObject()->getInteger("recognized_count_after"), 1);
    }
    if (field(F, "evidence_status") ==
        "selected_comparison_source_occurrence") {
      ++Occurrences;
      EXPECT_EQ(field(F, "source_id"), SourceID);
      EXPECT_EQ(field(F, "artifact_id"), SourceInput);
      EXPECT_FALSE(field(F, "node_id").empty());
    }
  }
  EXPECT_TRUE(Counts);
  EXPECT_EQ(Occurrences, 2u);
  EXPECT_NE(field(report(A), "report_markdown").find("before/after: 1/1"),
            std::string::npos);
  const auto Before = encode(metadata());
  EXPECT_EQ(code(create(Object{{"schema_version", 1},
                               {"package_behavior_id", AfterBID},
                               {"package_diff_id", DiffID},
                               {"package_behavior_diff_id",
                                field(BD, "package_behavior_diff_id")}})),
            "package_assessment_behavior_mismatch");
  EXPECT_EQ(encode(metadata()), Before);
#endif
}

TEST_F(WebPackageAssessmentSDK,
       NativeCLIComposesAndReportsWithoutExternalPATH) {
#ifndef NEVERD_WEB_TEST_CLI
  GTEST_SKIP() << "Native CLI target not configured";
#else
  const std::string CLI = NEVERD_WEB_TEST_CLI;
  const auto Path = Root.string(), Out = Path + ".stdout",
             Err = Path + ".stderr";
  std::vector<std::string> Env{"PATH=/nonexistent",
                               "TMPDIR=" + Root.parent_path().string()};
  std::vector<llvm::StringRef> Environment;
  for (const auto &Entry : Env)
    Environment.push_back(Entry);
  const std::array<std::optional<llvm::StringRef>, 3> Redirects{
      llvm::StringRef(""), Out, Err};
  for (bool Markdown : {false, true}) {
    std::vector<llvm::StringRef> Args{CLI,  "web",        "package-assessment",
                                      Path, "1",          "package-json",
                                      "--", "--registry", "2",
                                      "3",  "4",          "--advisory",
                                      "5",  "3"};
    if (Markdown)
      Args.push_back("--markdown");
    ASSERT_EQ(llvm::sys::ExecuteAndWait(CLI, Args, Environment, Redirects, 30),
              0);
    std::ifstream Stream(Out);
    const std::string Text((std::istreambuf_iterator<char>(Stream)), {});
    EXPECT_EQ(Text.find("CANARY"), Text.npos);
    EXPECT_EQ(Text.find("YWJj"), Text.npos);
    if (Markdown) {
      EXPECT_EQ(Text.find("# Offline package evidence review"), 0u);
      EXPECT_NE(Text.find("declared_identity_original_inventory_unbound"),
                Text.npos);
      EXPECT_NE(Text.find("declared_version_match"), Text.npos);
    } else {
      EXPECT_NE(Text.find("\"package_assessment_id\""), Text.npos);
      EXPECT_NE(Text.find("\"safety_verdict\":\"not_assessed\""), Text.npos);
    }
  }
#endif
}
} // namespace

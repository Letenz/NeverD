//===- PackageAdvisorySDKTests.cpp - Offline advisory surface checks
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision, paging, aggregate cache limits and standalone CLI counterexamples.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
using Value = llvm::json::Value;
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  EXPECT_EQ(Text.find("CANARY"), std::string::npos);
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
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *Error = V.getAsObject()->getObject("error");
  return Error ? Error->getString("code").value_or("").str() : "";
}
std::string encode(Value V) { return llvm::formatv("{0}", V).str(); }
const char *Advisory = R"({"schema_version":"1.9.1","id":"CANARY_GHSA",
  "modified":"2026-10-10T00:00:00Z","affected":[{
    "package":{"ecosystem":"npm","name":"CANARY_PACKAGE"},
    "ranges":[{"type":"SEMVER","events":[
      {"introduced":"0"},{"fixed":"2.0.0"}]}]}],
  "severity":[{"type":"CVSS_V3","score":"CANARY_SCORE"}]})";

class WebPackageAdvisorySDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  std::string Revision, Input, Descriptor, Manifest, Package;
  void write(const char *Name, std::string_view Text) {
    std::ofstream Out(Root / Name, std::ios::binary);
    Out << Text;
  }
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-advisory-sdk", Dir));
    Root = Dir.str().str();
    write("a.advisory", Advisory);
    write("b.descriptor", R"({"schema_version":1,"source":"CANARY_FEED",
      "retrieved_at":"2026-10-11T12:00:00Z","as_of":"2026-10-11T11:00:00Z"})");
    write("c.package", R"({"name":"CANARY_PACKAGE","version":"1.0.0"})");
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open();
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    if (Root.empty())
      return;
    std::filesystem::remove(Root.string() + ".stdout", EC);
    std::filesystem::remove(Root.string() + ".stderr", EC);
    std::filesystem::remove_all(Root, EC);
  }
  std::string artifact(uint64_t Index) {
    const auto P = take(neverd_web_artifacts_json(S, Revision.data(),
                                                  Revision.size(), Index, 1));
    const auto *Items = P.getAsObject()->getArray("items");
    return Items && Items->size() == 1 ? field((*Items)[0], "artifact_id") : "";
  }
  void open() {
    const auto Path = Root.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    const auto M =
        take(neverd_web_import_commit_json(S, Token.data(), Token.size()));
    ASSERT_EQ(field(M, "status"), "ok");
    Revision = field(M, "revision");
    Input = artifact(1);
    Descriptor = artifact(2);
    Manifest = artifact(3);
    Package.clear();
  }
  Value inventory(const char *Kind = "package-json") {
    const auto R = take(neverd_web_packages_analyze_json(
        S, Revision.data(), Revision.size(), Manifest.data(), Manifest.size(),
        Kind, std::strlen(Kind)));
    Package = field(R, "package_analysis_id");
    return R;
  }
  Value import(const std::string &Artifact = {}, const std::string &Desc = {}) {
    const auto &A = Artifact.empty() ? Input : Artifact;
    const auto &D = Desc.empty() ? Descriptor : Desc;
    return take(neverd_web_package_advisory_import_json(
        S, Revision.data(), Revision.size(), A.data(), A.size(), D.data(),
        D.size()));
  }
  Value rows(const std::string &ID, const char *Kind, uint64_t Offset = 0,
             uint64_t Limit = 512) {
    return take(neverd_web_package_advisory_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind,
        std::strlen(Kind), Offset, Limit));
  }
  Value match(const std::vector<std::string> &IDs) {
    llvm::json::Array Selections;
    for (const auto &ID : IDs)
      Selections.emplace_back(ID);
    const auto Options = encode(llvm::json::Object{
        {"schema_version", 1}, {"advisory_ids", std::move(Selections)}});
    return take(neverd_web_package_advisories_match_json(
        S, Revision.data(), Revision.size(), Package.data(), Package.size(),
        Options.data(), Options.size()));
  }
  Value matches(const std::string &ID, uint64_t Offset = 0,
                uint64_t Limit = 512) {
    return take(neverd_web_package_advisories_match_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
  Value metadata() { return take(neverd_web_metadata_json(S)); }
};

TEST_F(WebPackageAdvisorySDK, OriginalAnchorsResolveWithoutJavascriptEvidence) {
  const auto A = import();
  ASSERT_EQ(field(A, "status"), "ok");
  EXPECT_EQ(field(A, "artifact_id"), Input);
  const auto ID = field(A, "package_advisory_id");
  const auto Records = rows(ID, "affected");
  ASSERT_EQ(Records.getAsObject()->getArray("items")->size(), 1u);
  const auto Entry = field(Records.getAsObject()->getArray("items")->front(),
                           "affected_entry_id");
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_advisory_count"), 1);
  ASSERT_EQ(field(inventory(), "status"), "ok");
  const auto Empty = match({});
  EXPECT_EQ(field(Empty, "selection_status"), "not_supplied");
  const auto M = match({ID});
  ASSERT_EQ(field(M, "status"), "ok");
  EXPECT_EQ(M.getAsObject()->getInteger("declared_version_match_count"), 1);
  EXPECT_FALSE(
      M.getAsObject()->getBoolean("exploitability_verified").value_or(true));
  const auto Page = matches(field(M, "package_advisory_matches_id"));
  const auto &Record = Page.getAsObject()->getArray("items")->front();
  EXPECT_EQ(field(Record, "match_status"), "declared_version_match");
  EXPECT_EQ(Record.getAsObject()
                ->getArray("matched_affected_entry_ids")
                ->front()
                .getAsString(),
            Entry);
  const auto Packages = take(neverd_web_package_records_json(
      S, Revision.data(), Revision.size(), Package.data(), Package.size(),
      "packages", 8, 0, 512));
  EXPECT_EQ(
      field(Record, "package_id"),
      field(Packages.getAsObject()->getArray("items")->front(), "package_id"));
  const auto Meta = metadata();
  EXPECT_EQ(Meta.getAsObject()->getInteger("source_inventory_count"), 0);
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_evidence_descriptor_count"),
            1);
  EXPECT_EQ(field(import(), "package_advisory_id"), ID);
  EXPECT_EQ(field(match({ID}), "package_advisory_matches_id"),
            field(M, "package_advisory_matches_id"));
  EXPECT_EQ(field(rows(ID, "severities"), "severity_status"),
            "supplied_unvalidated");
}

TEST_F(WebPackageAdvisorySDK,
       InvalidRequestsAndNewImportsNeverRebindOldEvidence) {
  const auto A = import();
  const auto ID = field(A, "package_advisory_id");
  inventory();
  const auto M = field(match({ID}), "package_advisory_matches_id");
  EXPECT_EQ(code(rows(ID, "affected", 0, 0)), "invalid_page");
  EXPECT_EQ(code(rows(ID, "affected", 0, 513)), "invalid_page");
  EXPECT_EQ(code(rows(ID, "affected", 2)), "invalid_page");
  EXPECT_EQ(code(rows(ID, "CANARY_KIND")), "invalid_record_kind");
  EXPECT_EQ(code(matches(M, 2)), "invalid_page");
  EXPECT_EQ(code(match({ID, ID})), "package_advisory_duplicate_selection");
  EXPECT_EQ(code(match({std::string(64, '0')})), "unknown_package_advisory");
  EXPECT_EQ(code(take(neverd_web_package_advisory_import_json(
                S, Revision.data(), Revision.size(), nullptr, 64,
                Descriptor.data(), Descriptor.size()))),
            "invalid_buffer");
  for (const auto *Options :
       {R"({"schema_version":1,"fetch":true})",
        R"({"schema_version":1,"advisory_ids":[1]})",
        R"({"schema_version":1,"advisory_ids":["CANARY"]})"})
    EXPECT_EQ(code(take(neverd_web_package_advisories_match_json(
                  S, Revision.data(), Revision.size(), Package.data(),
                  Package.size(), Options, std::strlen(Options)))),
              "invalid_package_advisory_options");
  const auto Old = Revision;
  open();
  EXPECT_EQ(code(rows(ID, "affected")), "unknown_package_advisory");
  EXPECT_EQ(code(matches(M)), "unknown_package_advisory_matches");
  const auto Meta = metadata();
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_advisory_count"), 0);
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_evidence_retained_bytes"),
            0);
  EXPECT_EQ(code(take(neverd_web_package_advisory_import_json(
                S, Old.data(), Old.size(), Input.data(), Input.size(),
                Descriptor.data(), Descriptor.size()))),
            "stale_revision");
  EXPECT_EQ(field(import(), "status"), "ok");
}

TEST_F(WebPackageAdvisorySDK,
       SourceDescriptorsAndMatchSelectionsHaveSeparateIdentities) {
  write("z.descriptor", R"({"schema_version":1})");
  open();
  inventory();
  const auto A = import();
  const auto B = import(Input, artifact(4));
  const auto Before = field(A, "package_advisory_id");
  const auto After = field(B, "package_advisory_id");
  EXPECT_NE(Before, After);
  EXPECT_EQ(field(B, "temporal_status"), "feed_time_coverage_unavailable");
  const auto M = match({Before, After});
  ASSERT_EQ(field(M, "status"), "ok");
  EXPECT_EQ(field(match({After, Before}), "package_advisory_matches_id"),
            field(M, "package_advisory_matches_id"));
  EXPECT_EQ(field(match({}), "status"), "ok");
  EXPECT_EQ(code(match({Before})),
            "package_advisory_match_cache_budget_exceeded");
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_advisory_matches_count"),
      2);
}

TEST_F(WebPackageAdvisorySDK,
       UncomposedImportsConsumeCapacityAndRepeatedInputsReuseIt) {
  for (unsigned I = 0; I < 17; ++I)
    write(("z" + std::to_string(100 + I)).c_str(), Advisory);
  open();
  const auto First = field(import(), "package_advisory_id");
  for (unsigned I = 0; I < 15; ++I)
    EXPECT_EQ(field(import(artifact(4 + I)), "status"), "ok");
  const auto Before = encode(metadata());
  EXPECT_EQ(code(import(artifact(19))),
            "package_advisory_import_cache_budget_exceeded");
  EXPECT_EQ(field(import(), "package_advisory_id"), First);
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_descriptor_count"),
      1);
  open();
  EXPECT_EQ(field(import(artifact(19)), "status"), "ok");
}

TEST_F(WebPackageAdvisorySDK,
       AggregateMetadataRefusalPreservesUsableImportCapacity) {
  llvm::json::Array Versions;
  const std::string Version = "1.0.0+" + std::string(90, 'x');
  for (unsigned I = 0; I < 8192; ++I)
    Versions.emplace_back(Version);
  const auto Large = encode(llvm::json::Object{
      {"id", "CANARY_ID"},
      {"modified", "2026-10-10T00:00:00Z"},
      {"affected",
       llvm::json::Array{llvm::json::Object{
           {"package", llvm::json::Object{{"ecosystem", "npm"},
                                          {"name", "CANARY_PACKAGE"}}},
           {"versions", std::move(Versions)}}}}});
  ASSERT_LT(Large.size(), 1024u * 1024u);
  for (unsigned I = 0; I < 8; ++I)
    write(("z" + std::to_string(100 + I)).c_str(), Large);
  open();
  unsigned Accepted = 0;
  std::string First;
  for (; Accepted < 8; ++Accepted) {
    const auto Before = encode(metadata());
    const auto A = import(artifact(4 + Accepted));
    if (field(A, "status") == "error") {
      EXPECT_EQ(code(A), "package_evidence_retained_budget_exceeded");
      EXPECT_EQ(encode(metadata()), Before);
      break;
    }
    ASSERT_EQ(field(A, "status"), "ok");
    if (!Accepted)
      First = field(A, "package_advisory_id");
  }
  EXPECT_GT(Accepted, 0u);
  EXPECT_LT(Accepted, 8u);
  EXPECT_EQ(field(import(artifact(4)), "package_advisory_id"), First);
  EXPECT_EQ(field(import(), "status"), "ok");
  const auto Meta = metadata();
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_advisory_count"),
            Accepted + 1);
  EXPECT_EQ(Meta.getAsObject()->getInteger("package_evidence_descriptor_count"),
            1);
  EXPECT_LE(Meta.getAsObject()
                ->getInteger("package_evidence_retained_bytes")
                .value_or(0),
            8 * 1024 * 1024);
}

TEST_F(WebPackageAdvisorySDK,
       AggregateMatchWorkFailureDoesNotConsumeResultCache) {
  const std::string Version = std::string(1000, '9') + ".0.0";
  llvm::json::Object Packages;
  for (unsigned I = 0; I < 4096; ++I)
    Packages[I ? "node_modules/p" + std::to_string(I) : ""] =
        llvm::json::Object{{"name", "CANARY_PACKAGE"}, {"version", Version}};
  write("c.package",
        encode(llvm::json::Object{{"lockfileVersion", 3},
                                  {"packages", std::move(Packages)}}));
  open();
  ASSERT_EQ(field(inventory("npm-lock"), "status"), "ok");
  const auto ID = field(import(), "package_advisory_id");
  const auto Before = encode(metadata());
  EXPECT_EQ(code(match({ID})), "package_metadata_work_budget_exceeded");
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(field(match({}), "status"), "ok");
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_advisory_matches_count"),
      1);
}

TEST_F(WebPackageAdvisorySDK,
       MalformedImportsLeaveNoDescriptorOrAdvisoryBehind) {
  write("z.invalid", R"({"id":"CANARY_GHSA","modified":"2026-10-10T00:00:00Z",
    "affected":[{"ranges":[{"type":"SEMVER","events":null}]}]})");
  open();
  const auto Before = encode(metadata());
  EXPECT_EQ(code(import(artifact(4))), "package_metadata_invalid_array");
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(field(import(), "status"), "ok");
}

TEST_F(WebPackageAdvisorySDK,
       PagesRetainAllOriginalEntriesAndPackageAssociations) {
  llvm::json::Array Affected;
  for (unsigned I = 0; I < 600; ++I)
    Affected.emplace_back(llvm::json::Object{
        {"package",
         llvm::json::Object{{"ecosystem", "npm"}, {"name", "CANARY_PACKAGE"}}},
        {"versions", llvm::json::Array{"1.0.0"}}});
  write("a.advisory",
        encode(llvm::json::Object{{"id", "CANARY_ID"},
                                  {"modified", "2026-10-10T00:00:00Z"},
                                  {"affected", std::move(Affected)}}));
  open();
  const auto ID = field(import(), "package_advisory_id");
  const auto P = rows(ID, "affected");
  EXPECT_EQ(P.getAsObject()->getArray("items")->size(), 512u);
  EXPECT_EQ(P.getAsObject()->getInteger("next_offset"), 512);
  const auto End = rows(ID, "affected", 512);
  EXPECT_EQ(End.getAsObject()->getArray("items")->size(), 88u);
  EXPECT_EQ(End.getAsObject()->getBoolean("page_complete"), true);
  inventory();
  const auto Records =
      matches(field(match({ID}), "package_advisory_matches_id"));
  const auto &Associations = *Records.getAsObject()
                                  ->getArray("items")
                                  ->front()
                                  .getAsObject()
                                  ->getArray("matched_affected_entry_ids");
  EXPECT_EQ(Associations.size(), 600u);
  EXPECT_EQ(Associations[512].getAsString(),
            field(End.getAsObject()->getArray("items")->front(),
                  "affected_entry_id"));
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebPackageAdvisorySDK,
       CLIUsesOnlyCapturedInputsWithUnusableExternalToolPath) {
  const auto Path = Root.string();
  const auto Out = Path + ".stdout", Err = Path + ".stderr";
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  inventory();
  const auto ID = field(import(), "package_advisory_id");
  const auto Expected = match({ID});
  for (const bool Selected : {false, true}) {
    llvm::SmallVector<llvm::StringRef> Args{
        NEVERD_WEB_TEST_CLI, "web", "package-advisories", Path, "3",
        "package-json"};
    if (Selected)
      Args.push_back("1:2");
    std::ofstream(Out, std::ios::trunc).close();
    std::ofstream(Err, std::ios::trunc).close();
    ASSERT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                        Redirects, 30),
              0);
    std::ifstream Stream(Out);
    std::string Line;
    unsigned Lines = 0, Summaries = 0;
    while (std::getline(Stream, Line)) {
      EXPECT_EQ(Line.find("CANARY"), Line.npos);
      auto Parsed = llvm::json::parse(Line);
      ASSERT_TRUE(bool(Parsed));
      const auto *Object = Parsed->getAsObject();
      ASSERT_NE(Object, nullptr);
      ++Lines;
      if (Object->get("package_advisory_matches_id") && !Object->get("items")) {
        ++Summaries;
        EXPECT_EQ(field(*Parsed, "selection_status"),
                  Selected ? "explicit_advisory_selection" : "not_supplied");
        if (Selected)
          EXPECT_EQ(field(*Parsed, "package_advisory_matches_id"),
                    field(Expected, "package_advisory_matches_id"));
      }
    }
    EXPECT_EQ(Summaries, 1u);
    EXPECT_EQ(Lines, Selected ? 7u : 4u);
    EXPECT_EQ(std::filesystem::file_size(Err), 0u);
  }
}
#endif
} // namespace

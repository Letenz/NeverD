//===- WebTests.cpp - MCP evidence and shared API parity ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise real offline analysis through closed MCP schemas, without runtimes.
///
//===----------------------------------------------------------------------===//

#include "../../../unittests/web/BunFixture.h"
#include "../../../unittests/web/NexeFixture.h"
#include "../../../unittests/web/PkgFixture.h"
#include "../../../unittests/web/SEAFixture.h"
#include "../../../unittests/web/WailsFixture.h"
#include "../../../unittests/web/ZipFixture.h"
#include "TestSupport.h"
#include "neverd-web-mcp/WebTools.h"

#include <iostream>
#include <memory>
#include <set>

using namespace neverd::mcp;
using namespace neverd::mcp::test;
namespace transport = neverd::transport;
namespace {
template <class F> void invalid(F Run) {
  try {
    Run();
  } catch (const RpcError &E) {
    require(E.Code == -32602, "Tool arguments used the wrong RPC error");
    return;
  }
  throw std::runtime_error("Invalid tool argument was admitted");
}
Json take(const char *Text) {
  std::unique_ptr<const char, decltype(&neverd_free_string)> Owner(
      Text, neverd_free_string);
  require(Text != nullptr, "Missing C API result");
  return transport::parseJson(Text);
}

class Harness {
public:
  WebTools Tools;
  neverd::web_client::Backend Reference;
  const std::vector<std::string> Inputs;
  std::string Revision;
  explicit Harness(std::vector<std::string> Paths)
      : Tools(Paths), Inputs(std::move(Paths)) {}

  Json get(const char *Operation, Json Args = Json::object()) {
    return evidence(Tools.call("neverd_" + std::string(Operation), Args));
  }
  Json same(const char *Operation, Json Args) {
    auto Actual = get(Operation, Args);
    Args["schema_version"] = 1;
    require(Actual == Reference.execute(Operation, Args),
            "MCP evidence differs from the shared worker/C API adapter");
    return Actual;
  }
  Json fail(const char *Operation, Json Args, const char *Code) {
    auto Error =
        evidence(Tools.call("neverd_" + std::string(Operation), Args), true);
    require(Error.at("error").at("code") == Code,
            "Wrong tool failure diagnostic");
    return Error;
  }
  Json import(std::size_t Index, std::size_t ExpectedArtifacts = 1) {
    const auto Preview = get("web_import_preview", {{"input_index", Index}});
    const auto Other =
        Reference.execute("web_import_preview",
                          {{"schema_version", 1}, {"path", Inputs.at(Index)}});
    const auto Commit = get("web_import_commit",
                            {{"preview_token", Preview.at("preview_token")}});
    require(Commit == Reference.execute(
                          "web_import_commit",
                          {{"schema_version", 1},
                           {"preview_token", Other.at("preview_token")}}),
            "MCP publication differs from the backend");
    Revision = Commit.at("revision").get<std::string>();
    same("web_metadata", Json::object());
    const auto Items = same("web_artifacts", {{"revision", Revision}});
    require(Items.at("items").size() == ExpectedArtifacts,
            "Unexpected captured artifact count");
    return Items.at("items")[0].at("artifact_id");
  }
};

void discovery(Harness &H, const Json &Caps) {
  std::set<std::string> Names;
  Json Params = Json::object();
  for (;;) {
    const auto Page = H.Tools.list(Params);
    redacted(Page);
    for (const auto &T : Page.at("tools"))
      require(Names.insert(T.at("name").get<std::string>()).second,
              "Discovery duplicated a tool");
    if (!Page.contains("nextCursor"))
      break;
    Params["cursor"] = Page.at("nextCursor");
  }
  require(Names.contains("neverd_web_capabilities"),
          "Capabilities tool missing");
  if (Caps.value("status", "") != "ok") {
    require(Names.size() == 1, "Omitted backend exposed analysis tools");
    return;
  }
  for (const auto &Operation : Caps.at("operations")) {
    const auto Name = Operation.get<std::string>();
    const bool Omitted = Name == "bun_export" || Name == "wails_export" ||
                         Name == "native_analyze" ||
                         Name == "source_readable_export" ||
                         Name == "source_rewrite_export";
    require(Names.contains("neverd_web_" + Name) != Omitted,
            "MCP catalog drifted from the documented backend subset");
  }
}

void clientSourceFeatures(Harness &H) {
  const auto Artifact = H.import(0);
  const auto S = H.same("web_source_analyze", {{"revision", H.Revision},
                                               {"artifact_id", Artifact},
                                               {"source_type", "script"}});
  const Json Args{{"revision", H.Revision}, {"source_id", S.at("source_id")}};
  const auto N = H.same("web_source_navigation_analyze", Args);
  const auto &F = N.at("client_features");
  require(F.at("profile") == "javascript-client-syntax-features-v1" &&
              F.at("status") == "partial" && F.at("record_count") == 7 &&
              F.at("feature_inventory_complete") == false &&
              F.at("api_identity") == "not_authenticated" &&
              F.at("effective_policy") == "not_analyzed",
          "MCP promoted client syntax candidates or lost their coverage");
  auto Page = Args;
  Page["record_kind"] = "features";
  Page["limit"] = 1;
  const auto First = H.same("web_source_navigation_records", Page);
  require(First.at("items").size() == 1 && First.at("next_offset") == 1 &&
              First.at("items")[0].at("confidence") == "source_syntax_only" &&
              First.at("items")[0].at("callback_source_link") ==
                  "lexical_declaration_or_initializer" &&
              First.at("items")[0].at("runtime_callback_target") ==
                  "not_proven",
          "MCP changed client callback syntax or paging");
  Page["offset"] = 1;
  Page["limit"] = 512;
  const auto Rest = H.same("web_source_navigation_records", Page);
  require(Rest.at("items").size() == 6 && Rest.at("page_complete") == true,
          "MCP lost client pattern continuation");
  redacted(N);
  redacted(First);
  redacted(Rest);
  Page["execute"] = true;
  invalid([&] { H.get("web_source_navigation_records", Page); });
}

void sourceAndMaps(Harness &H) {
  const auto Artifact = H.import(0);
  const Json SourceArgs{{"revision", H.Revision},
                        {"artifact_id", Artifact},
                        {"source_type", "module"}};
  const auto Source = H.same("web_source_analyze", SourceArgs);
  const Json Args{{"revision", H.Revision},
                  {"source_id", Source.at("source_id")}};
  H.same("web_source_nodes", Args);
  for (const auto *Op :
       {"web_source_bindings_analyze", "web_source_semantics_analyze",
        "web_source_modules_analyze", "web_source_bundles_analyze",
        "web_source_navigation_analyze", "web_interfaces_analyze"})
    H.same(Op, Args);
  H.same("web_source_semantic_records", Args);
  for (const auto *Pass : {"string_recovery", "constant_folding"}) {
    auto RewriteArgs = Args;
    RewriteArgs["pass"] = Pass;
    const auto Rewrite = H.same("web_source_rewrite_analyze", RewriteArgs);
    require(Rewrite.at("analysis_status") == "verified_local_rewrites" &&
                Rewrite.at("accepted_count") == 1 &&
                Rewrite.at("whole_program_equivalence") == false &&
                Rewrite.at("source_reflection_preserved") == false,
            "MCP changed rewrite admission or its qualifications");
    const auto Records = H.same(
        "web_source_rewrite_records",
        {{"revision", H.Revision}, {"rewrite_id", Rewrite.at("rewrite_id")}});
    redacted(Rewrite);
    redacted(Records);
    RewriteArgs["path"] = "CANARY";
    invalid([&] { H.get("web_source_rewrite_analyze", RewriteArgs); });
  }
  const auto Interfaces = H.same("web_interfaces_analyze", Args);
  require(Interfaces.at("interface_count") == 7 &&
              Interfaces.at("xhr_correlation") == false &&
              Interfaces.at("fetch_member_correlation") == false &&
              Interfaces.at("profile") == "fetch-websocket-xhr-syntax-v3",
          "MCP lost XHR syntax evidence or changed correlation claims");
  const auto Sites =
      H.same("web_interface_records",
             {{"revision", H.Revision},
              {"analysis_id", Interfaces.at("interface_analysis_id")}});
  const auto &Items = Sites.at("items");
  require(Items[1].at("kind") == "xhr_open" &&
              Items[2].at("header_kind") == "sensitive" &&
              Items[3].at("body_status") == "argument_syntax" &&
              Items[4].at("handler_status") == "value_syntax" &&
              Items[1].at("receiver_binding_id") ==
                  Items[4].at("receiver_binding_id"),
          "MCP changed XHR source links");
  require(Items[5].at("kind") == "fetch_member_call" &&
              Items[5].at("receiver_status") == "member_spelling_syntax_only" &&
              Items[5].at("receiver_node_id").is_string() &&
              Items[5].at("method") == "unknown" &&
              Items[6].at("arguments_status") == "spread_unresolved" &&
              Items[6].at("url_node_id").is_null() &&
              Items[6].at("method") == "unknown",
          "MCP promoted a member spelling or invented spread arguments");
  const auto Relations =
      H.same("web_interface_relations_analyze",
             {{"revision", H.Revision},
              {"analysis_id", Interfaces.at("interface_analysis_id")}});
  require(Relations.at("profile") == "request-source-relations-v1" &&
              Relations.at("relation_count") == 2 &&
              Relations.at("response_api_identity") ==
                  "member_spelling_unverified" &&
              Relations.at("har_correlation") == false,
          "MCP changed request relations or response identity");
  const Json RelationArgs{{"revision", H.Revision},
                          {"relations_id", Relations.at("relations_id")}};
  const auto RelationPage =
      H.same("web_interface_relation_records", RelationArgs);
  require(RelationPage.at("items")[0].at("kind") ==
                  "await_result_const_binding" &&
              RelationPage.at("items")[1].at("kind") ==
                  "bound_result_member_call" &&
              RelationPage.at("items")[1].at("member_spelling") == "json" &&
              RelationPage.at("items")[0].at("binding_id") ==
                  RelationPage.at("items")[1].at("binding_id"),
          "MCP changed result binding anchors");
  redacted(Relations);
  redacted(RelationPage);
  auto UnsafeRelation = RelationArgs;
  UnsafeRelation["execute"] = true;
  invalid([&] { H.get("web_interface_relation_records", UnsafeRelation); });
  auto Bindings = Args;
  Bindings["record_kind"] = "bindings";
  H.same("web_source_binding_records", Bindings);
  const auto Preview = H.same("web_source_view_preview", Args);
  const auto View = H.same("web_source_view_commit",
                           {{"revision", H.Revision},
                            {"preview_token", Preview.at("preview_token")}});
  const Json ViewArgs{{"revision", H.Revision},
                      {"view_id", View.at("view_id")}};
  H.same("web_source_view_records", ViewArgs);
  auto Chunk = ViewArgs;
  Chunk["byte_offset"] = "0";
  Chunk["byte_limit"] = 65536;
  H.same("web_source_view_chunk", Chunk);
  auto Anchor = Args;
  Anchor.update({{"byte_offset", "0"},
                 {"byte_length", "5"},
                 {"view_id", View.at("view_id")}});
  H.same("web_source_anchor", Anchor);
  auto Unsafe = Args;
  Unsafe["options"] = {
      {"reviewed_ranges",
       Json::array({{{"byte_offset", "0"}, {"byte_length", "100"}}})}};
  invalid([&] { H.get("web_source_view_preview", Unsafe); });
  auto Stale = Args;
  Stale["revision"] = "0";
  H.fail("web_source_nodes", Stale, "stale_revision");

  const auto MapArtifact = H.import(6);
  const auto Map =
      H.same("web_source_map_analyze",
             {{"revision", H.Revision}, {"artifact_id", MapArtifact}});
  const Json MapArgs{{"revision", H.Revision}, {"map_id", Map.at("map_id")}};
  H.same("web_source_map_sources", MapArgs);
  H.same("web_source_map_segments", MapArgs);
}

void scopeRename(Harness &H) {
  const auto Artifact = H.import(0);
  const auto Source = H.same("web_source_analyze", {{"revision", H.Revision},
                                                    {"artifact_id", Artifact},
                                                    {"source_type", "script"}});
  const Json Args{{"revision", H.Revision},
                  {"source_id", Source.at("source_id")},
                  {"pass", "scope_renaming"}};
  const auto A = H.same("web_source_rewrite_analyze", Args);
  require(A.at("profile") == "javascript-scope-rename-v1" &&
              A.at("analysis_status") == "verified_local_rewrites" &&
              A.at("accepted_count") == 4 &&
              A.at("value_verification") == "not_applicable" &&
              A.at("value_analysis_id").is_null() &&
              A.at("binding_verification") ==
                  "same_scopes_declarations_and_reference_resolution" &&
              A.at("original_name_recovery") == false,
          "MCP changed lexical rename evidence");
  const auto Page =
      H.same("web_source_rewrite_records",
             {{"revision", H.Revision}, {"rewrite_id", A.at("rewrite_id")}});
  require(Page.at("items").size() == 5, "Rename records missing");
  for (const auto &R : Page.at("items"))
    require(R.contains("binding_id") && R.contains("identifier_role") &&
                !R.contains("before") && !R.contains("after"),
            "Rename records lost their private evidence boundary");
  redacted(A);
  redacted(Page);
  require(Page.dump().find("neverd_arg_") == std::string::npos &&
              Page.dump().find("neverd_local_") == std::string::npos,
          "MCP exposed generated names");
}

void booleanBranches(Harness &H) {
  const auto Artifact = H.import(0);
  const auto Source = H.same("web_source_analyze", {{"revision", H.Revision},
                                                    {"artifact_id", Artifact},
                                                    {"source_type", "script"}});
  const Json Args{{"revision", H.Revision},
                  {"source_id", Source.at("source_id")},
                  {"pass", "boolean_branches"}};
  const auto A = H.same("web_source_rewrite_analyze", Args);
  require(A.at("profile") == "javascript-boolean-branches-v1" &&
              A.at("analysis_status") == "verified_local_rewrites" &&
              A.at("accepted_count") == 1 &&
              A.at("value_verification") == "constant_boolean_condition_only" &&
              A.at("binding_verification") ==
                  "same_scopes_declarations_and_retained_references" &&
              A.at("retained_expression_effects") ==
                  "preserved_not_assumed_absent" &&
              A.at("statements_rewritten") == false,
          "MCP changed Boolean branch evidence");
  const auto Page =
      H.same("web_source_rewrite_records",
             {{"revision", H.Revision}, {"rewrite_id", A.at("rewrite_id")}});
  require(Page.at("items").size() == 1, "Branch record missing");
  const auto &R = Page.at("items")[0];
  require(R.contains("condition_node_id") && R.contains("retained_node_id") &&
              R.at("selected_operand") == "consequent" &&
              !R.contains("binding_id") && !R.contains("identifier_role") &&
              !R.contains("before") && !R.contains("after"),
          "Branch receipt changed or exposed private source");
  redacted(A);
  redacted(Page);
}

void archives(Harness &H, bool HasParser) {
  auto Artifact = H.import(1);
  const Json Args{{"revision", H.Revision},
                  {"artifact_id", Artifact},
                  {"profile", "node-sea-22.15.0-blob-le64-v1"}};
  const auto SEA = H.same("web_sea_extract", Args);
  require(SEA.at("asset_count") == 2, "SEA assets missing");
  auto Page = Json{{"revision", H.Revision},
                   {"extraction_id", SEA.at("extraction_id")},
                   {"limit", 1}};
  const auto First = H.same("web_sea_records", Page);
  require(First.at("items").size() == 1, "SEA records did not paginate");
  Page["offset"] = 1;
  H.same("web_sea_records", Page);
  if (HasParser) {
    const auto Source = H.same("web_source_analyze",
                               {{"revision", H.Revision},
                                {"artifact_id", SEA.at("source_artifact_id")},
                                {"source_type", "commonjs"}});
    H.same("web_source_anchor", {{"revision", H.Revision},
                                 {"source_id", Source.at("source_id")},
                                 {"byte_offset", "0"},
                                 {"byte_length", "6"}});
  }
  Artifact = H.import(2);
  const auto Bun = H.same(
      "web_bun_extract", {{"revision", H.Revision}, {"artifact_id", Artifact}});
  require(Bun.at("module_count") == 3, "Bun modules missing");
  const auto Modules =
      H.same("web_bun_records", {{"revision", H.Revision},
                                 {"extraction_id", Bun.at("extraction_id")},
                                 {"record_kind", "modules"}});
  require(Modules.at("items").size() == 3, "Bun metadata page missing");
  invalid([&] { H.get("web_bun_export", {{"revision", H.Revision}}); });

  Artifact = H.import(3);
  const auto Package =
      H.same("web_packages_analyze", {{"revision", H.Revision},
                                      {"artifact_id", Artifact},
                                      {"input_kind", "package-json"}});
  H.same("web_package_records",
         {{"revision", H.Revision},
          {"analysis_id", Package.at("package_analysis_id")},
          {"record_kind", "scripts"}});
}

void offlineAssessments() {
  Fixture F;
  F.write("a.package", R"({"name":"CANARY","version":"1.0.0"})");
  F.write("b.registry", R"({"name":"CANARY","version":"1.0.0","dist":{
    "integrity":"sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=",
    "signatures":[{"keyid":"CANARY_KEY","sig":"YWJj"}]}})");
  F.write("c.descriptor", R"({"schema_version":1,"source":"CANARY_SOURCE",
    "retrieved_at":"2026-10-11T12:00:00Z"})");
  F.write("d.original", "abc");
  Harness H({F.Root.string()});
  H.import(0, 5);
  const auto Artifacts = H.same("web_artifacts", {{"revision", H.Revision}});
  const auto &Items = Artifacts.at("items");
  const auto P = H.same("web_packages_analyze",
                        {{"revision", H.Revision},
                         {"artifact_id", Items[1].at("artifact_id")},
                         {"input_kind", "package-json"}});
  const auto T = H.same("web_package_attestation_import",
                        {{"revision", H.Revision},
                         {"artifact_id", Items[2].at("artifact_id")},
                         {"descriptor_artifact_id", Items[3].at("artifact_id")},
                         {"input_kind", "registry-dist"}});
  const auto I = H.same("web_package_integrity_verify",
                        {{"revision", H.Revision},
                         {"artifact_id", Items[4].at("artifact_id")},
                         {"declaration_id", Items[2].at("artifact_id")}});
  Json Selection{
      {"revision", H.Revision},
      {"package_analysis_id", P.at("package_analysis_id")},
      {"integrity_ids", Json::array({I.at("integrity_id")})},
      {"attestation_ids", Json::array({T.at("package_attestation_id")})}};
  const auto A = H.same("web_package_assessment_create", Selection);
  require(A.at("safety_verdict") == "not_assessed" &&
              A.at("publisher_authenticated") == false &&
              A.at("installed_tree_verified") == false,
          "MCP assessment upgraded a selected digest into trust");
  const auto ID = A.at("package_assessment_id"), Root = A.at("review_head_id");
  Json Page{{"revision", H.Revision},
            {"package_assessment_id", ID},
            {"review_head", Root}};
  const auto Findings = H.same("web_package_assessment_findings", Page);
  require(Findings.at("items")[0].at("disposition_source") == "not_reviewed",
          "MCP invented an analyst disposition");
  const auto Finding = Findings.at("items")[0].at("finding_id");
  Json Review{{"revision", H.Revision},
              {"package_assessment_id", ID},
              {"expected_review_head", Root},
              {"finding_id", Finding},
              {"disposition", "needs_review"}};
  const auto U = H.same("web_package_review_append", Review);
  require(U.at("decision_count") == 1 && U.at("replayed") == false,
          "MCP lost the first immutable review decision");
  require(H.same("web_package_review_append", Review).at("replayed") == true,
          "MCP double-charged a last-request replay");
  H.fail("web_package_assessment_findings", Page, "stale_package_review_head");
  Page["review_head"] = U.at("review_head_id");
  const auto Updated = H.same("web_package_assessment_findings", Page);
  require(Updated.at("items")[0].at("finding_id") == Finding &&
              Updated.at("items")[0].at("disposition") == "needs_review",
          "MCP changed the evidence finding when reviewing it");
  const auto Decisions = H.same("web_package_review_decisions", Page);
  require(Decisions.at("items").size() == 1 &&
              Decisions.at("items")[0].at("assertion_source") == "caller",
          "MCP upgraded an analyst assertion into analyzer proof");
  const Json Journal{{"schema_version", 1},
                     {"profile", "offline-package-review-journal-v1"},
                     {"package_assessment_id", ID},
                     {"review_root_id", U.at("review_root_id")},
                     {"review_head_id", U.at("review_head_id")},
                     {"decisions", Decisions.at("items")}};
  Json Restore{{"revision", H.Revision},
               {"package_assessment_id", ID},
               {"expected_review_head", U.at("review_head_id")},
               {"journal_json", Journal.dump()}};
  const auto Restored = H.same("web_package_review_restore", Restore);
  require(Restored.at("replayed") == true &&
              Restored.at("restored_events") == 0,
          "MCP charged a journal replay twice");
  Restore["raw"] = true;
  invalid([&] { H.get("web_package_review_restore", Restore); });
  const auto Report = H.same("web_package_assessment_report", Page);
  redacted(Report);
  require(Report.at("report_format") == "markdown" &&
              Report.at("report_markdown").get<std::string>().find("YWJj") ==
                  std::string::npos,
          "MCP report disclosed a private signature");
  Selection["notes"] = "CANARY";
  invalid([&] { H.get("web_package_assessment_create", Selection); });
  Review["disposition"] = "safe";
  invalid([&] { H.get("web_package_review_append", Review); });
  H.import(0, 5);
  H.fail("web_package_assessment_report", Page, "stale_revision");
  Page["revision"] = H.Revision;
  H.fail("web_package_review_decisions", Page, "unknown_package_assessment");
}

void offlineAdvisories() {
  Fixture F;
  F.write("a.advisory",
          R"({"id":"CANARY_GHSA","modified":"2026-10-10T00:00:00Z",
    "affected":[{"package":{"ecosystem":"npm","name":"CANARY_PACKAGE"},
      "versions":["1.0.0"]}],"severity":[{"type":"CVSS_V3","score":"CANARY"}]})");
  F.write("b.descriptor", R"({"schema_version":1,"source":"CANARY_FEED",
    "retrieved_at":"2026-10-11T12:00:00Z","as_of":"2026-10-11T11:00:00Z"})");
  F.write("c.package", R"({"name":"CANARY_PACKAGE","version":"1.0.0"})");
  Harness H({F.Root.string()});
  H.import(0, 4);
  const auto Artifacts = H.same("web_artifacts", {{"revision", H.Revision}});
  const auto &Items = Artifacts.at("items");
  Json Import{{"revision", H.Revision},
              {"artifact_id", Items[1].at("artifact_id")},
              {"descriptor_artifact_id", Items[2].at("artifact_id")}};
  const auto Advisory = H.same("web_package_advisory_import", Import);
  require(Advisory.at("publisher_authenticated") == false &&
              Advisory.at("freshness_assessed") == false,
          "MCP upgraded captured metadata into authentication or freshness");
  const auto ID = Advisory.at("package_advisory_id");
  for (const auto *Kind : {"affected", "severities"})
    H.same("web_package_advisory_records", {{"revision", H.Revision},
                                            {"package_advisory_id", ID},
                                            {"record_kind", Kind}});
  const auto P = H.same("web_packages_analyze",
                        {{"revision", H.Revision},
                         {"artifact_id", Items[3].at("artifact_id")},
                         {"input_kind", "package-json"}});
  Json Selection{{"revision", H.Revision},
                 {"package_analysis_id", P.at("package_analysis_id")}};
  for (const bool Selected : {false, true}) {
    if (Selected)
      Selection["advisory_ids"] = Json::array({ID});
    const auto M = H.same("web_package_advisories_match", Selection);
    require(M.at("declared_version_match_count") == (Selected ? 1 : 0) &&
                M.at("exploitability_verified") == false &&
                M.at("analysis_status") == "partial",
            "MCP lost version selection or its explicit evidence boundary");
    const auto Records = H.same(
        "web_package_advisories_match_records",
        {{"revision", H.Revision},
         {"package_advisory_matches_id", M.at("package_advisory_matches_id")}});
    require(Records.at("items").size() == (Selected ? 1 : 0),
            "MCP changed selected advisory match rows");
    if (Selected)
      require(Records.at("items")[0].at("match_status") ==
                  "declared_version_match",
              "MCP changed the admitted version membership");
  }
  Selection["advisory_ids"] = Json::array({ID, ID});
  H.fail("web_package_advisories_match", Selection,
         "package_advisory_duplicate_selection");
  Import["fetch"] = true;
  invalid([&] { H.get("web_package_advisory_import", Import); });
  const auto Old = H.Revision;
  H.import(0, 4);
  H.fail("web_package_advisory_records",
         {{"revision", Old},
          {"package_advisory_id", ID},
          {"record_kind", "affected"}},
         "stale_revision");
  H.fail("web_package_advisory_records",
         {{"revision", H.Revision},
          {"package_advisory_id", ID},
          {"record_kind", "affected"}},
         "unknown_package_advisory");
}

void offlineAttestations() {
  Fixture F;
  F.write("a.statement", R"({"_type":"https://in-toto.io/Statement/v1",
    "subject":[{"name":"CANARY","digest":{"sha256":
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}}],
    "predicateType":"CANARY_PREDICATE"})");
  F.write("b.descriptor", R"({"schema_version":1,"source":"CANARY"})");
  F.write("c.original", "abc");
  F.write("d.registry", R"({"name":"CANARY","version":"1.0.0",
    "dist":{"signatures":[{"sig":"Zm9v","keyid":"CANARY"}]}})");
  F.write("e.envelope", R"({"payloadType":"application/vnd.in-toto+json",
    "payload":"Zh==","signatures":[]})");
  Harness H({F.Root.string()});
  H.import(0, 6);
  const auto Artifacts = H.same("web_artifacts", {{"revision", H.Revision}});
  const auto Items = Artifacts.at("items");
  Json Import{{"revision", H.Revision},
              {"artifact_id", Items[1].at("artifact_id")},
              {"descriptor_artifact_id", Items[2].at("artifact_id")},
              {"input_kind", "intoto-statement"}};
  const auto A = H.same("web_package_attestation_import", Import);
  require(A.at("signature_verification") == "not_performed" &&
              A.at("publisher_authenticated") == false &&
              A.at("slsa_level_verified") == false,
          "MCP authenticated unverified metadata");
  const auto ID = A.at("package_attestation_id");
  Json Digests;
  for (const auto *Kind : {"signatures", "subjects", "digests"}) {
    const auto Page = H.same("web_package_attestation_records",
                             {{"revision", H.Revision},
                              {"package_attestation_id", ID},
                              {"record_kind", Kind}});
    if (std::string_view(Kind) == "digests")
      Digests = Page;
  }
  const auto M =
      H.same("web_package_attestation_digest_match",
             {{"revision", H.Revision},
              {"artifact_id", Items[3].at("artifact_id")},
              {"package_attestation_id", ID},
              {"digest_id", Digests.at("items")[0].at("digest_id")}});
  require(
      M.at("integrity_status") == "match" &&
          M.at("package_inventory_bound") == false &&
          M.at("publisher_authenticated") == false,
      "MCP changed an original digest comparison into package authentication");
  H.same("web_package_attestation_digest_result",
         {{"revision", H.Revision},
          {"package_attestation_digest_match_id",
           M.at("package_attestation_digest_match_id")}});
  Import["artifact_id"] = Items[4].at("artifact_id");
  Import["input_kind"] = "registry-dist";
  const auto Registry = H.same("web_package_attestation_import", Import);
  const auto Signatures =
      H.same("web_package_attestation_records",
             {{"revision", H.Revision},
              {"package_attestation_id", Registry.at("package_attestation_id")},
              {"record_kind", "signatures"}});
  require(Signatures.at("items")[0].at("encoding_status") ==
                  "admitted_standard_base64" &&
              Signatures.at("items")[0].at("signature_verification") ==
                  "not_performed",
          "MCP changed registry signature admission");
  Import["artifact_id"] = Items[5].at("artifact_id");
  Import["input_kind"] = "dsse-intoto";
  require(
      H.same("web_package_attestation_import", Import).at("payload_status") ==
          "invalid_base64",
      "MCP accepted nonzero Base64 residual bits");
  Import["verify"] = true;
  invalid([&] { H.get("web_package_attestation_import", Import); });
  const auto Old = H.Revision;
  H.import(0, 6);
  H.fail("web_package_attestation_records",
         {{"revision", Old},
          {"package_attestation_id", ID},
          {"record_kind", "subjects"}},
         "stale_revision");
  H.fail("web_package_attestation_digest_result",
         {{"revision", H.Revision},
          {"package_attestation_digest_match_id",
           M.at("package_attestation_digest_match_id")}},
         "unknown_package_attestation_digest_match");
}

void passiveInputs(Harness &H) {
  auto Artifact = H.import(4);
  const auto Preview = H.same(
      "web_har_preview", {{"revision", H.Revision}, {"artifact_id", Artifact}});
  const Json Page{{"revision", H.Revision},
                  {"capture_id", Preview.at("capture_id")}};
  H.fail("web_har_records", Page, "har_capture_not_committed");
  H.same("web_har_commit", {{"revision", H.Revision},
                            {"preview_token", Preview.at("preview_token")}});
  H.same("web_har_records", Page);

  Artifact = H.import(5);
  const auto Stream = H.same("web_stream_preview",
                             {{"revision", H.Revision},
                              {"artifact_id", Artifact},
                              {"profile", "recorded-jsonrpc-2.0-jsonl-v1"}});
  const Json StreamPage{{"revision", H.Revision},
                        {"capture_id", Stream.at("stream_capture_id")}};
  H.fail("web_stream_records", StreamPage, "stream_capture_not_committed");
  H.same("web_stream_commit", {{"revision", H.Revision},
                               {"preview_token", Stream.at("preview_token")}});
  const auto Records = H.same("web_stream_records", StreamPage);
  require(Records.at("items").size() == 2 &&
              Records.at("protocol_negotiation_verified") == false,
          "Stream facts or evidence class changed");
}

void zipArchive(Harness &H, bool HasParser) {
  const auto Artifact = H.import(7);
  const auto A = H.same(
      "web_package_archive_extract",
      {{"revision", H.Revision}, {"artifact_id", Artifact}, {"format", "zip"}});
  const auto Page =
      H.same("web_package_archive_records",
             {{"revision", H.Revision}, {"archive_id", A.at("archive_id")}});
  require(A.at("profile") == "zip32-local-central-v1" &&
              Page.at("items")[1].at("unavailable_reason") == "encrypted",
          "MCP lost ZIP profile or unavailable member evidence");
  const auto P = H.same("web_packages_analyze",
                        {{"revision", H.Revision},
                         {"artifact_id", Page.at("items")[0].at("member_id")},
                         {"input_kind", "package-json"}});
  H.same("web_package_records", {{"revision", H.Revision},
                                 {"analysis_id", P.at("package_analysis_id")},
                                 {"record_kind", "scripts"}});
  if (HasParser) {
    const auto Source =
        H.same("web_source_analyze",
               {{"revision", H.Revision},
                {"artifact_id", Page.at("items")[2].at("member_id")},
                {"source_type", "commonjs"}});
    const Json Args{{"revision", H.Revision},
                    {"source_id", Source.at("source_id")}};
    const auto Behavior = H.same("web_node_behavior_analyze", Args);
    require(Behavior.at("record_count") == 1 &&
                Behavior.at("runtime_api_verified") == false,
            "MCP changed Node syntax qualification");
    const auto Records =
        H.same("web_node_behavior_records",
               {{"revision", H.Revision},
                {"node_behavior_id", Behavior.at("node_behavior_id")}});
    require(Records.at("items")[0].at("api_class") == "child_process.execFile",
            "MCP lost the fixed Node API class");
    const Json Selection{{"revision", H.Revision},
                         {"package_analysis_id", P.at("package_analysis_id")},
                         {"source_ids", Json::array({Source.at("source_id")})}};
    const auto Aggregate = H.same("web_package_behavior_analyze", Selection);
    require(Aggregate.at("selection_complete") == false &&
                Aggregate.at("record_count") == 1,
            "MCP changed selected package coverage");
    const Json InstallArgs{
        {"revision", H.Revision},
        {"package_analysis_id", P.at("package_analysis_id")}};
    const auto Empty = H.same("web_package_install_analyze", InstallArgs);
    require(Empty.at("source_count") == 0 && Empty.at("script_count") == 3,
            "MCP requires invented source selection for install declarations");
    const auto EmptyRows =
        H.same("web_package_install_records",
               {{"revision", H.Revision},
                {"package_install_id", Empty.at("package_install_id")},
                {"record_kind", "scripts"}});
    require(EmptyRows.at("items")[0].at("status") ==
                    "entry_source_unselected" &&
                EmptyRows.at("items")[1].at("status") ==
                    "not_captured_or_unavailable" &&
                EmptyRows.at("items")[2].at("status") ==
                    "not_captured_or_unavailable",
            "MCP overstates absent or encrypted file coverage");
    auto SelectedInstall = InstallArgs;
    SelectedInstall["package_behavior_id"] =
        Aggregate.at("package_behavior_id");
    const auto Trace = H.same("web_package_install_analyze", SelectedInstall);
    require(Trace.at("association_count") == 1 &&
                Trace.at("reachability_verified") == false &&
                Trace.at("lifecycle_activation_verified") == false,
            "MCP installation association changed its confidence");
    for (const auto *Kind : {"scripts", "sources", "requests", "associations"})
      redacted(H.same("web_package_install_records",
                      {{"revision", H.Revision},
                       {"package_install_id", Trace.at("package_install_id")},
                       {"record_kind", Kind}}));
    for (const auto *Kind : {"sources", "records"})
      redacted(
          H.same("web_package_behavior_records",
                 {{"revision", H.Revision},
                  {"package_behavior_id", Aggregate.at("package_behavior_id")},
                  {"record_kind", Kind}}));
    auto Bad = Selection;
    const auto Base = H.same("web_packages_compare",
                             {{"revision", H.Revision},
                              {"before_id", P.at("package_analysis_id")},
                              {"after_id", P.at("package_analysis_id")}});
    Json Compare{{"revision", H.Revision},
                 {"package_diff_id", Base.at("package_diff_id")}};
    const auto Missing = H.same("web_package_behavior_diff_analyze", Compare);
    require(Missing.at("source_pair_count") == 0 &&
                Missing.at("before_selection") == "not_supplied",
            "MCP invented comparison coverage without selected sources");
    Compare["before_package_behavior_id"] = Aggregate.at("package_behavior_id");
    Compare["after_package_behavior_id"] = Aggregate.at("package_behavior_id");
    const auto Diff = H.same("web_package_behavior_diff_analyze", Compare);
    require(Diff.at("compared_pair_count") == 1 &&
                Diff.at("occurrence_count") == 2 &&
                Diff.at("call_correspondence_verified") == false,
            "MCP lost independent before/after syntax occurrences");
    for (const auto *Kind : {"pairs", "patterns", "occurrences"})
      redacted(H.same(
          "web_package_behavior_diff_records",
          {{"revision", H.Revision},
           {"package_behavior_diff_id", Diff.at("package_behavior_diff_id")},
           {"record_kind", Kind}}));
    Compare["path"] = "CANARY_PATH";
    invalid([&] { H.get("web_package_behavior_diff_analyze", Compare); });
    Bad["execute"] = true;
    invalid([&] { H.get("web_package_behavior_analyze", Bad); });
  }
  for (const auto *Kind : {"nwjs", "vsix"}) {
    const auto M = H.same("web_desktop_manifest_analyze",
                          {{"revision", H.Revision},
                           {"artifact_id", Page.at("items")[0].at("member_id")},
                           {"input_kind", Kind}});
    require(M.at("runtime_entry_verified") == false &&
                M.at("framework_verified") == false,
            "MCP changed desktop evidence classification");
  }
}
} // namespace

int main() {
  try {
    Fixture F;
    std::vector<std::string> Inputs;
    for (const auto *Name :
         {"source", "sea", "bun", "package", "har", "stream", "map", "zip"})
      Inputs.push_back(F.path(Name));
    // The configured paths deliberately do not exist yet: construction must
    // neither capture nor reject them before an explicit preview request.
    Harness H(Inputs);
    const auto Caps = H.get("web_capabilities");
    auto Backend = Caps.at("backend");
    discovery(H, Backend);
    if (Backend.value("status", "") != "ok") {
      invalid([&] { H.get("web_import_preview", {{"input_index", 0}}); });
      std::cout << "MCP backend-omitted capabilities passed\n";
      return 0;
    }
    Backend.erase("operation_prefix");
    Backend.erase("payload_schema_version");
    require(Backend == take(neverd_web_capabilities_json()),
            "MCP changed C API capabilities");
    if (Backend.value("input_reader", "") == "unavailable")
      return 77;
    H.fail("web_metadata", Json::object(), "no_project");
    H.fail("web_import_preview", {{"input_index", 0}}, "input_unavailable");
    invalid([&] {
      H.get("web_import_preview", {{"input_index", 0}, {"path", "CANARY"}});
    });
    invalid([&] { H.get("web_import_preview", {{"input_index", 8}}); });
    invalid([&] {
      H.get("web_native_analyze",
            {{"revision", "1"}, {"handoff_id", "CANARY"}});
    });
    F.write(
        "source",
        "const CANARY_RESULT=await "
        "fetch('https://CANARY/a?token=CANARY');CANARY_RESULT.json(); const "
        "privateName = 'CANARY'; while(true) {};"
        "const CANARY_XHR = new XMLHttpRequest();"
        "CANARY_XHR.open('GET','https://CANARY/a');"
        "CANARY_XHR.setRequestHeader('Authorization','CANARY');"
        "CANARY_XHR.send('CANARY');"
        "CANARY_XHR.onload = CANARY_CALLBACK;"
        "this.fetch('https://CANARY/a', {method:'POST'});"
        "fetch(...CANARY_ARGUMENTS);"
        "const recovered='CAN'+'ARY_RECOVERED';const n=1+2;");
    F.write("sea", neverd::web::sea_test::blob(12));
    F.write("bun", neverd::web::test::BunFixture().Bytes);
    F.write(
        "package",
        R"({"name":"CANARY","version":"1.0.0","scripts":{"postinstall":"CANARY"}})");
    F.write(
        "har",
        R"({"log":{"version":"1.2","entries":[{"request":{"method":"GET","url":"https://CANARY/a?token=CANARY","headers":[{"name":"Authorization","value":"CANARY"}]},"response":{"status":200,"content":{"text":"CANARY"}}}]}})");
    F.write(
        "stream",
        R"({"session":"CANARY","direction":"client_to_server","message":{"jsonrpc":"2.0","id":"CANARY","method":"CANARY","params":{"key":"CANARY"}}})"
        "\n"
        R"({"session":"CANARY","direction":"server_to_client","message":{"jsonrpc":"2.0","id":"CANARY","result":"CANARY"}})"
        "\n");
    F.write(
        "map",
        R"({"version":3,"sources":["CANARY.js"],"sourcesContent":["throw 'CANARY';"],"mappings":"AAAA"})");
    F.write(
        "zip",
        neverd::web::test::ZipFixture(
            {{"package.json",
              R"({"name":"CANARY","scripts":{"install":"node install.js CANARY","postinstall":"node opaque","preinstall":"node CANARY_ABSENT.js"}})"},
             {"opaque", "CANARY", 0, 1},
             {"install.js",
              "require('node:child_process').execFile('CANARY',options);"}})
            .Bytes);
    const auto &Ops = Backend.at("operations");
    const bool HasParser =
        std::find(Ops.begin(), Ops.end(), "source_analyze") != Ops.end();
    if (HasParser) {
      sourceAndMaps(H);
      F.write("source", "function CANARY_NAME(CANARY_ARG){"
                        "const CANARY_LOCAL=CANARY_ARG;return CANARY_LOCAL;}");
      scopeRename(H);
      F.write("source", "function CANARY_NAME(CANARY_ARG){"
                        "return true ? CANARY_ARG.method() : CANARY_UNUSED;}");
      booleanBranches(H);
      F.write("source",
              "function CANARY_CALLBACK(v){return v;}"
              "server.registerTool('CANARY_TOOL',{},CANARY_CALLBACK);"
              "if(checkPermissions('CANARY'))server.callTool('CANARY');"
              "const settings={permissions:{deny:['CANARY']},"
              "mcpServers:{CANARY:{}},hooks:{PreToolUse:[]}};");
      clientSourceFeatures(H);
    } else
      invalid([&] { H.get("web_source_analyze", Json::object()); });
    archives(H, HasParser);
    if (std::find(Ops.begin(), Ops.end(), "nexe_extract") != Ops.end()) {
      F.write("sea",
              neverd::web::nexe_test::bundle(
                  {{"CANARY.js", "const x='CANARY';"}, {"data", "CANARY"}}));
      const auto A = H.import(1);
      const Json Args{{"revision", H.Revision},
                      {"artifact_id", A},
                      {"profile", "nexe-4.0.0-rc.7-eof-resources-v1"}};
      const auto E = H.same("web_nexe_extract", Args);
      require(E.at("member_count") == 2 &&
                  E.at("signature_status") == "not_checked",
              "nexe evidence differs");
      const Json Page{{"revision", H.Revision},
                      {"extraction_id", E.at("extraction_id")},
                      {"record_kind", "members"},
                      {"limit", 1}};
      const auto M = H.same("web_nexe_records", Page);
      require(M.at("next_offset") == 1, "nexe pages lost continuation");
      H.fail("web_nexe_records",
             {{"revision", H.Revision},
              {"extraction_id", E.at("extraction_id")},
              {"record_kind", "unknown"}},
             "invalid_record_kind");
      auto Invalid = Args;
      Invalid["execute"] = true;
      invalid([&] { H.get("web_nexe_extract", Invalid); });
      if (HasParser) {
        const auto Source =
            H.same("web_source_analyze",
                   {{"revision", H.Revision},
                    {"artifact_id", M.at("items")[0].at("selection_id")},
                    {"source_type", "commonjs"}});
        H.same("web_source_anchor", {{"revision", H.Revision},
                                     {"source_id", Source.at("source_id")},
                                     {"byte_offset", "6"},
                                     {"byte_length", "1"}});
      }
    }
    if (std::find(Ops.begin(), Ops.end(), "pkg_extract") != Ops.end()) {
      F.write("sea",
              neverd::web::pkg_test::bundle(
                  {{"CANARY.js", "const x='CANARY';"}, {"data", "CANARY"}}));
      const auto A = H.import(1);
      const Json Args{{"revision", H.Revision},
                      {"artifact_id", A},
                      {"profile", "pkg-5.8.1-uncompressed-vfs-v1"}};
      const auto E = H.same("web_pkg_extract", Args);
      require(E.at("member_count") == 4 &&
                  E.at("signature_status") == "not_checked",
              "pkg evidence differs");
      const Json Page{{"revision", H.Revision},
                      {"extraction_id", E.at("extraction_id")},
                      {"record_kind", "members"},
                      {"limit", 1}};
      const auto M = H.same("web_pkg_records", Page);
      require(M.at("next_offset") == 1, "pkg pages lost continuation");
      H.fail("web_pkg_records",
             {{"revision", H.Revision},
              {"extraction_id", E.at("extraction_id")},
              {"record_kind", "unknown"}},
             "invalid_record_kind");
      auto Invalid = Args;
      Invalid["execute"] = true;
      invalid([&] { H.get("web_pkg_extract", Invalid); });
      if (HasParser) {
        const auto Source =
            H.same("web_source_analyze",
                   {{"revision", H.Revision},
                    {"artifact_id", M.at("items")[0].at("selection_id")},
                    {"source_type", "commonjs"}});
        H.same("web_source_anchor", {{"revision", H.Revision},
                                     {"source_id", Source.at("source_id")},
                                     {"byte_offset", "6"},
                                     {"byte_length", "1"}});
      }
    }
    if (std::find(Ops.begin(), Ops.end(), "wails_extract") != Ops.end()) {
      F.write("sea", neverd::web::wails_test::elf());
      const auto A = H.import(1);
      const Json Args{
          {"revision", H.Revision},
          {"artifact_id", A},
          {"profile", "wails-2.11.0-go-1.25.7-linux-x64-elf-embedfs-v1"}};
      const auto E = H.same("web_wails_extract", Args);
      require(E.at("table_count") == 1 && E.at("member_count") == 3 &&
                  E.at("asset_server_binding") == "not_checked",
              "Wails stored table evidence differs");
      const Json Page{{"revision", H.Revision},
                      {"extraction_id", E.at("extraction_id")},
                      {"record_kind", "members"},
                      {"offset", 1},
                      {"limit", 1}};
      const auto M = H.same("web_wails_records", Page);
      require(M.at("next_offset") == 2 &&
                  M.dump().find("CANARY") == std::string::npos,
              "Wails page differs or exposes private names");
      auto Invalid = Args;
      Invalid["execute"] = true;
      invalid([&] { H.get("web_wails_extract", Invalid); });
      auto InvalidPage = Page;
      InvalidPage["limit"] = 129;
      invalid([&] { H.get("web_wails_records", InvalidPage); });
      H.fail("web_wails_records",
             {{"revision", H.Revision},
              {"extraction_id", E.at("extraction_id")},
              {"record_kind", "paths"}},
             "invalid_record_kind");
      if (HasParser) {
        const auto S =
            H.same("web_source_analyze",
                   {{"revision", H.Revision},
                    {"artifact_id", M.at("items")[0].at("selection_id")},
                    {"source_type", "module"}});
        require(S.at("parse_status") == "parsed",
                "Wails member source is unavailable");
      }
    }
    offlineAdvisories();
    offlineAttestations();
    offlineAssessments();
    passiveInputs(H);
    for (const auto &Analysis : Backend.at("analysis"))
      if (Analysis.at("kind") == "package_archive" &&
          Analysis.value("available", false))
        zipArchive(H, HasParser);
    if (HasParser) {
      for (bool LockOnly : {false, true}) {
        F.write(
            "package",
            LockOnly
                ? R"({"lockfileVersion":3,"packages":{"":{"hasInstallScript":true}}})"
                : R"({"scripts":{"install":"node CANARY_MISSING.js"}})");
        const auto Artifact = H.import(3);
        const auto P =
            H.same("web_packages_analyze",
                   {{"revision", H.Revision},
                    {"artifact_id", Artifact},
                    {"input_kind", LockOnly ? "npm-lock" : "package-json"}});
        const auto A =
            H.same("web_package_install_analyze",
                   {{"revision", H.Revision},
                    {"package_analysis_id", P.at("package_analysis_id")}});
        const auto Rows =
            H.same("web_package_install_records",
                   {{"revision", H.Revision},
                    {"package_install_id", A.at("package_install_id")},
                    {"record_kind", "scripts"}});
        require(
            A.at("source_count") == 0 && A.at("association_count") == 0 &&
                Rows.at("items")[0].at("status") ==
                    (LockOnly ? "manifest_not_supplied"
                              : "no_directory_inventory"),
            "MCP requires invented evidence for manifest-only install traces");
        if (LockOnly)
          require(Rows.at("items")[0].at("script_id").is_null(),
                  "Lock install presence invented a script anchor");
      }
    }
    WebTools Isolated(Inputs);
    const auto Error =
        evidence(Isolated.call("neverd_web_metadata", Json::object()), true);
    require(Error.at("error").at("code") == "no_project",
            "Sessions shared publication state");
    std::cout << "MCP capability, evidence, redaction, parity and session "
                 "contracts passed; parser="
              << HasParser << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
  } catch (const RpcError &E) {
    std::cerr << E.Code << ": " << E.Message << '\n';
  }
  return 1;
}

//===- CatalogTests.cpp - MCP tool admission contracts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capability filtering and argument boundaries independent of analysis.
///
//===----------------------------------------------------------------------===//

#include "neverd-web-mcp/Catalog.h"

#include <functional>
#include <iostream>
#include <set>

using namespace neverd::mcp;
namespace {
void require(bool Condition, const char *Message) {
  if (!Condition)
    throw std::runtime_error(Message);
}
template <class F> void refuses(F Run) {
  try {
    Run();
  } catch (const RpcError &E) {
    require(E.Code == -32602, "Wrong catalog refusal classification");
    return;
  }
  throw std::runtime_error("Invalid tool request was admitted");
}
Json capabilities(Json Operations) {
  return {{"status", "ok"}, {"operations", std::move(Operations)}};
}
void capabilitySubset() {
  for (const auto &Caps : {Json::object(), Json{{"status", "error"}}}) {
    Catalog Disabled(Caps, 1);
    require(Disabled.names() == Json::array({"neverd_web_capabilities"}),
            "Disabled backend advertised unavailable tools");
  }
  auto Caps = capabilities(
      {"import_preview", "source_map_analyze", "source_analyze",
       "native_analyze", "bun_export", "wails_export", "source_readable_export",
       "source_rewrite_export", "unknown_future_operation"});
  Catalog C(Caps, 0);
  require(C.names() == Json::array({"neverd_web_capabilities",
                                    "neverd_web_source_analyze",
                                    "neverd_web_source_map_analyze"}),
          "Catalog exposed unconfigured, unknown or excluded operations");
  Catalog Inputs(Caps, 2);
  const auto &Import = Inputs.find("neverd_web_import_preview");
  require(Import.payload({{"input_index", 1}}).at("schema_version") == 1,
          "Launch input selection failed");
  refuses([&] { Import.payload({{"input_index", 2}}); });
  refuses([&] { Import.payload({{"path", "CANARY_PATH"}}); });
}
void closedArguments() {
  Catalog C(capabilities({"source_view_preview",
                          "source_view_chunk",
                          "import_commit",
                          "electron_ipc_analyze",
                          "source_rewrite_analyze",
                          "source_rewrite_records",
                          "node_behavior_analyze",
                          "node_behavior_records",
                          "package_behavior_analyze",
                          "package_behavior_records",
                          "package_install_analyze",
                          "package_install_records",
                          "package_behavior_diff_analyze",
                          "package_behavior_diff_records",
                          "package_advisory_import",
                          "package_advisory_records",
                          "package_advisories_match",
                          "package_advisories_match_records",
                          "package_attestation_import",
                          "package_attestation_records",
                          "package_attestation_digest_match",
                          "package_attestation_digest_result",
                          "package_assessment_create",
                          "package_assessment_findings",
                          "package_review_append",
                          "package_review_decisions",
                          "package_assessment_report",
                          "interface_relations_analyze",
                          "interface_relation_records",
                          "wails_extract",
                          "wails_records",
                          "tauri_extract",
                          "tauri_records"}),
            0);
  const auto &Wails = C.find("neverd_web_wails_extract");
  Json WailsArgs{{"revision", "1"}, {"artifact_id", "a"}, {"profile", "p"}};
  Wails.payload(WailsArgs);
  WailsArgs["execute"] = true;
  refuses([&] { Wails.payload(WailsArgs); });
  const auto &WailsPage = C.find("neverd_web_wails_records");
  Json WailsPageArgs{{"revision", "1"},
                     {"extraction_id", "e"},
                     {"record_kind", "members"},
                     {"limit", 128}};
  WailsPage.payload(WailsPageArgs);
  WailsPageArgs["limit"] = 129;
  refuses([&] { WailsPage.payload(WailsPageArgs); });
  const auto &Tauri = C.find("neverd_web_tauri_extract");
  Json TauriArgs{{"revision", "1"},
                 {"artifact_id", "a"},
                 {"profile", "p"},
                 {"initializer_address", "4294967552"}};
  Tauri.payload(TauriArgs);
  TauriArgs["table"] = Json::object();
  refuses([&] { Tauri.payload(TauriArgs); });
  refuses([&] {
    Tauri.payload({{"revision", "1"},
                   {"artifact_id", "a"},
                   {"profile", "p"},
                   {"initializer_address", "0x100000100"}});
  });
  const auto &TauriPage = C.find("neverd_web_tauri_records");
  TauriPage.payload(
      {{"revision", "1"}, {"extraction_id", "e"}, {"limit", 128}});
  refuses([&] {
    TauriPage.payload(
        {{"revision", "1"}, {"extraction_id", "e"}, {"limit", 129}});
  });
  const auto &Relations = C.find("neverd_web_interface_relations_analyze");
  Relations.payload({{"revision", "1"}, {"analysis_id", "a"}});
  refuses([&] { Relations.payload({{"revision", "1"}, {"source_id", "a"}}); });
  const auto &RelationPage = C.find("neverd_web_interface_relation_records");
  Json RelationArgs{{"revision", "1"}, {"relations_id", "r"}, {"limit", 128}};
  RelationPage.payload(RelationArgs);
  RelationArgs["limit"] = 129;
  refuses([&] { RelationPage.payload(RelationArgs); });
  RelationArgs["limit"] = 1;
  RelationArgs["execute"] = true;
  refuses([&] { RelationPage.payload(RelationArgs); });
  const auto &Assessment = C.find("neverd_web_package_assessment_create");
  Json AssessmentArgs{{"revision", "1"},
                      {"package_analysis_id", "p"},
                      {"integrity_ids", Json::array({std::string(64, 'a')})}};
  Assessment.payload(AssessmentArgs);
  for (const auto *Field : {"notes", "raw", "path", "execute", "score"}) {
    auto Extra = AssessmentArgs;
    Extra[Field] = "CANARY";
    refuses([&] { Assessment.payload(Extra); });
  }
  AssessmentArgs["attestation_ids"] =
      std::vector<std::string>(5, std::string(64, 'a'));
  refuses([&] { Assessment.payload(AssessmentArgs); });
  const auto &Review = C.find("neverd_web_package_review_append");
  Json ReviewArgs{{"revision", "1"},
                  {"package_assessment_id", "a"},
                  {"expected_review_head", "h"},
                  {"finding_id", "f"},
                  {"disposition", "needs_review"}};
  Review.payload(ReviewArgs);
  ReviewArgs["disposition"] = "safe";
  refuses([&] { Review.payload(ReviewArgs); });
  for (const auto *Operation : {"neverd_web_package_assessment_findings",
                                "neverd_web_package_review_decisions",
                                "neverd_web_package_assessment_report"}) {
    const auto &Page = C.find(Operation);
    Json Args{{"revision", "1"},
              {"package_assessment_id", "a"},
              {"review_head", "h"},
              {"limit", 512}};
    Page.payload(Args);
    Args.erase("review_head");
    refuses([&] { Page.payload(Args); });
  }
  const auto &View = C.find("neverd_web_source_view_preview");
  const auto &Attestation = C.find("neverd_web_package_attestation_import");
  const Json AttestationArgs{{"revision", "1"},
                             {"artifact_id", "a"},
                             {"descriptor_artifact_id", "d"},
                             {"input_kind", "registry-dist"}};
  Attestation.payload(AttestationArgs);
  for (const auto *Field : {"fetch", "key_path", "verify", "raw", "url"}) {
    auto Invalid = AttestationArgs;
    Invalid[Field] = "CANARY";
    refuses([&] { Attestation.payload(Invalid); });
  }
  auto MissingKind = AttestationArgs;
  MissingKind.erase("input_kind");
  refuses([&] { Attestation.payload(MissingKind); });
  const auto &Digest = C.find("neverd_web_package_attestation_digest_match");
  Digest.payload({{"revision", "1"},
                  {"artifact_id", "a"},
                  {"package_attestation_id", "d"},
                  {"digest_id", "s"}});
  const auto &Advisory = C.find("neverd_web_package_advisory_import");
  const Json ImportArgs{
      {"revision", "1"}, {"artifact_id", "a"}, {"descriptor_artifact_id", "d"}};
  Advisory.payload(ImportArgs);
  auto Remote = ImportArgs;
  Remote["fetch"] = true;
  refuses([&] { Advisory.payload(Remote); });
  const auto &Match = C.find("neverd_web_package_advisories_match");
  Json Matching{{"revision", "1"}, {"package_analysis_id", "p"}};
  Match.payload(Matching);
  Matching["advisory_ids"] = Json::array();
  Match.payload(Matching);
  Matching["advisory_ids"] = Json::array({std::string(64, 'a')});
  Match.payload(Matching);
  for (const auto &IDs :
       {Json::array({1}), Json::array({std::string("s\0x", 3)}),
        Json(std::vector<std::string>(17, std::string(64, 'a')))}) {
    Matching["advisory_ids"] = IDs;
    refuses([&] { Match.payload(Matching); });
  }
  const Json Valid{{"revision", "1"}, {"source_id", "s"}};
  require(View.payload(Valid).at("revision") == "1", "Valid view refused");
  for (const auto *Field : {"options", "reviewed_ranges", "path", "_meta"}) {
    auto Arguments = Valid;
    Arguments[Field] = Json::object();
    refuses([&] { View.payload(Arguments); });
  }
  for (const auto &Schema : {Json(2), Json(true), Json("1"), Json(1.0)}) {
    auto Arguments = Valid;
    Arguments["schema_version"] = Schema;
    refuses([&] { View.payload(Arguments); });
  }
  refuses([&] { View.payload({{"source_id", "s"}}); });
  refuses([&] { C.find("neverd_web_bun_export"); });
  refuses([&] { C.find("neverd_web_source_readable_export"); });
  refuses([&] { C.find("neverd_web_native_analyze"); });
  refuses([&] { C.find("neverd_web_source_rewrite_export"); });
  const auto &Rewrite = C.find("neverd_web_source_rewrite_analyze");
  auto RewriteArgs = Valid;
  RewriteArgs["pass"] = "string_recovery";
  Rewrite.payload(RewriteArgs);
  RewriteArgs["pass"] = "constant_folding";
  Rewrite.payload(RewriteArgs);
  RewriteArgs["pass"] = "scope_renaming";
  Rewrite.payload(RewriteArgs);
  RewriteArgs["pass"] = "boolean_branches";
  Rewrite.payload(RewriteArgs);
  RewriteArgs["pass"] = "execute";
  refuses([&] { Rewrite.payload(RewriteArgs); });
  const auto &IPC = C.find("neverd_web_electron_ipc_analyze");
  Json Selection{{"revision", "1"},
                 {"manifest_artifact_id", "a"},
                 {"source_ids", Json::array({"s"})}};
  IPC.payload(Selection);
  for (const auto &IDs :
       {Json::array(), Json::array({1}), Json::array({std::string("s\0x", 3)}),
        Json(std::vector<std::string>(17, "s"))}) {
    Selection["source_ids"] = IDs;
    refuses([&] { IPC.payload(Selection); });
  }
  const auto &Behavior = C.find("neverd_web_package_behavior_analyze");
  const auto &Diff = C.find("neverd_web_package_behavior_diff_analyze");
  Diff.payload({{"revision", "1"}, {"package_diff_id", "d"}});
  Diff.payload({{"revision", "1"},
                {"package_diff_id", "d"},
                {"before_package_behavior_id", "b"},
                {"after_package_behavior_id", "a"}});
  refuses([&] {
    Diff.payload(
        {{"revision", "1"}, {"package_diff_id", "d"}, {"execute", false}});
  });
  const auto &Install = C.find("neverd_web_package_install_analyze");
  Install.payload({{"revision", "1"}, {"package_analysis_id", "p"}});
  Install.payload({{"revision", "1"},
                   {"package_analysis_id", "p"},
                   {"package_behavior_id", "b"}});
  refuses([&] {
    Install.payload(
        {{"revision", "1"}, {"package_analysis_id", "p"}, {"execute", true}});
  });
  Json PackageSelection{{"revision", "1"},
                        {"package_analysis_id", "p"},
                        {"source_ids", Json::array({"s"})}};
  Behavior.payload(PackageSelection);
  for (const auto &IDs : {Json::array(), Json::array({1}),
                          Json(std::vector<std::string>(17, "s"))}) {
    auto Bad = PackageSelection;
    Bad["source_ids"] = IDs;
    refuses([&] { Behavior.payload(Bad); });
  }
  for (const auto *Field : {"execute", "origin", "path"}) {
    auto Bad = PackageSelection;
    Bad[Field] = "CANARY";
    refuses([&] { Behavior.payload(Bad); });
  }
}
void coordinateBounds() {
  {
    Catalog Desktop(capabilities({"desktop_manifest_analyze"}), 0);
    const auto &Tool = Desktop.find("neverd_web_desktop_manifest_analyze");
    Json Args{{"revision", "1"}, {"artifact_id", "a"}, {"input_kind", "nwjs"}};
    Tool.payload(Args);
    Args["input_kind"] = "vsix";
    Tool.payload(Args);
    Args["input_kind"] = "electron";
    refuses([&] { Tool.payload(Args); });
    Args["input_kind"] = "nwjs";
    Args["path"] = "CANARY_PATH";
    refuses([&] { Tool.payload(Args); });
  }
  Catalog C(capabilities({"source_anchor", "source_view_chunk", "artifacts"}),
            0);
  const auto &Anchor = C.find("neverd_web_source_anchor");
  Json Args{{"revision", "1"},
            {"source_id", "s"},
            {"byte_offset", "0"},
            {"byte_length", "18446744073709551615"}};
  Anchor.payload(Args);
  for (const auto &Offset : {"01", "-1", "+1", "1.0", "18446744073709551616"}) {
    Args["byte_offset"] = Offset;
    refuses([&] { Anchor.payload(Args); });
  }
  const auto &Page = C.find("neverd_web_artifacts");
  for (const auto &Limit :
       {Json(0), Json(-1), Json(true), Json(513), Json(1.0)})
    refuses([&] { Page.payload({{"revision", "1"}, {"limit", Limit}}); });
  Page.payload({{"revision", "1"}, {"limit", 512}});
  refuses([&] {
    C.find("neverd_web_source_view_chunk")
        .payload({{"revision", "1"},
                  {"view_id", "v"},
                  {"byte_offset", "0"},
                  {"byte_limit", 65537}});
  });
  Args["byte_offset"] = "0";
  Args["source_id"] = std::string(63, 's') + "中";
  refuses([&] { Anchor.payload(Args); });
}
void discoveryPages() {
  Catalog C(capabilities({"import_commit",      "metadata",
                          "artifacts",          "bun_extract",
                          "bun_records",        "source_analyze",
                          "source_nodes",       "source_map_analyze",
                          "source_map_sources", "source_map_segments",
                          "source_map_lookup",  "source_view_preview",
                          "source_view_commit", "source_view_records",
                          "source_view_chunk",  "native_open",
                          "native_metadata",    "sea_extract",
                          "sea_records",        "nexe_extract",
                          "nexe_records",       "pkg_extract",
                          "pkg_records"}),
            0);
  Json Params = Json::object();
  Json Names = Json::array();
  unsigned Pages = 0;
  do {
    const auto Page = C.list(Params);
    require(Page.at("tools").size() <= 16, "Discovery page exceeded bound");
    for (const auto &Tool : Page.at("tools")) {
      Names.push_back(Tool.at("name"));
      const auto &Schema = Tool.at("inputSchema");
      require(Schema.at("type") == "object" &&
                  Schema.at("additionalProperties") == false &&
                  Schema.at("properties").at("schema_version").at("const") == 1,
              "Catalog omitted the closed argument contract");
      require(Tool.at("annotations").at("openWorldHint") == false,
              "Offline tool declared external access");
    }
    ++Pages;
    if (!Page.contains("nextCursor"))
      break;
    Params["cursor"] = Page.at("nextCursor");
    require(Pages < 8, "Discovery cursor did not advance");
  } while (true);
  require(Names == C.names() && Pages == 2,
          "Discovery lost or duplicated tools");
  for (const auto &Cursor : {"", "web-tools-v1:0", "web-tools-v1:016",
                             "web-tools-v1:1", "web-tools-v1:32", "CANARY"})
    refuses([&] { C.list({{"cursor", Cursor}}); });
}
} // namespace

int main() {
  try {
    capabilitySubset();
    closedArguments();
    coordinateBounds();
    discoveryPages();
    std::cout << "4 MCP catalog contracts passed\n";
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}

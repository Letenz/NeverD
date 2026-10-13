//===- InterfaceSDKTests.cpp - Passive publication and CLI contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Redaction receipts, immutable evidence, bounded caches and CLI parity.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace {
using Value = llvm::json::Value;
void redacted(std::string_view Text) {
  EXPECT_EQ(Text.find("CANARY"), std::string_view::npos);
  EXPECT_EQ(Text.find("canary"), std::string_view::npos);
}
Value parse(std::string_view Text) {
  redacted(Text);
  auto V = llvm::json::parse(Text);
  if (!V) {
    llvm::consumeError(V.takeError());
    throw std::runtime_error("invalid response");
  }
  return std::move(*V);
}
Value take(const char *Owned) {
  if (!Owned)
    throw std::runtime_error("missing response");
  const std::string Text(Owned);
  neverd_free_string(Owned);
  return parse(Text);
}
std::string field(const Value &V, const char *Name) {
  return V.getAsObject()->getString(Name).value_or("").str();
}
std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}
class WebInterfaceSDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  std::string Revision;
  std::vector<std::string> Artifacts;
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture required";
#endif
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-interface-sdk", Dir));
    Root = Dir.str().str();
    for (unsigned I = 0; I < 5; ++I) {
      std::ofstream(Root / ("a" + std::to_string(I) + ".har"))
          << R"({"log":{"version":"1.2","creator":{"name":"CANARY_CREATOR",
        "version":"CANARY_VERSION"},"entries":[{
        "startedDateTime":"CANARY_TIMESTAMP",
        "request":{"method":"POST","url":"https://CANARY_HOST/CANARY_PATH?CANARY_QUERY=CANARY_VALUE",
          "headers":[{"name":"Authorization","value":"CANARY_AUTH"},
                     {"name":"CANARY_HEADER","value":"CANARY_VALUE"}],
          "cookies":[{"name":"CANARY_COOKIE","value":"CANARY_COOKIE_VALUE"}],
          "queryString":[{"name":"CANARY_QUERY","value":"CANARY_VALUE"}],
          "postData":{"text":"CANARY_BODY"}},
        "response":{"status":200,"statusText":"CANARY_STATUS",
          "redirectURL":"https://CANARY_REDIRECT",
          "headers":[{"name":"Set-Cookie","value":"CANARY_COOKIE"}],
          "content":{"text":"CANARY_RESPONSE","encoding":"CANARY_ENCODING"}},
        "_webSocketMessages":[{"data":"CANARY_FRAME"}],
        "_requestId":"CANARY_VENDOR"}],"comment":"CANARY_COMMENT_)"
          << I << "\"}}";
      std::ofstream(Root / ("b" + std::to_string(I) + ".js"))
          << "fetch('https://CANARY_HOST/"
             "CANARY_PATH?CANARY_QUERY=CANARY_VALUE',"
             "{method:'POST',headers:{CANARY_HEADER:'CANARY_VALUE'},"
             "body:'CANARY_BODY'});";
    }
    std::ofstream(Root / "bad.har") << "CANARY_MALFORMED";
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open();
  }
  void TearDown() override {
    neverd_web_session_destroy(S);
    std::error_code EC;
    std::filesystem::remove(Root.string() + ".stdout", EC);
    std::filesystem::remove(Root.string() + ".stderr", EC);
    std::filesystem::remove_all(Root, EC);
  }
  void open() {
    const auto Path = Root.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto T = field(P, "preview_token");
    Revision = field(take(neverd_web_import_commit_json(S, T.data(), T.size())),
                     "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &I : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(I, "artifact_id"));
    ASSERT_EQ(Artifacts.size(), 12u);
  }
  Value preview(unsigned I = 1) {
    const auto &A = Artifacts.at(I);
    return take(neverd_web_har_preview_json(S, Revision.data(), Revision.size(),
                                            A.data(), A.size()));
  }
  Value commit(std::string Token) {
    return take(neverd_web_har_commit_json(S, Revision.data(), Revision.size(),
                                           Token.data(), Token.size()));
  }
  Value records(std::string ID, uint64_t Offset = 0, uint64_t Limit = 128) {
    return take(neverd_web_har_records_json(S, Revision.data(), Revision.size(),
                                            ID.data(), ID.size(), Offset,
                                            Limit));
  }
  std::string admit(unsigned I = 1) {
    const auto P = preview(I);
    return field(commit(field(P, "preview_token")), "capture_id");
  }
  Value source(unsigned I = 6) {
    const auto &A = Artifacts.at(I);
    const auto Parsed = take(neverd_web_source_analyze_json(
        S, Revision.data(), Revision.size(), A.data(), A.size(), "module", 6));
    const auto ID = field(Parsed, "source_id");
    return take(neverd_web_interfaces_analyze_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value compare(std::string Source, std::string Capture) {
    return take(neverd_web_interfaces_compare_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size(),
        Capture.data(), Capture.size()));
  }
  Value relations(std::string ID) {
    return take(neverd_web_interface_relations_analyze_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value relationRecords(std::string ID, uint64_t Offset = 0,
                        uint64_t Limit = 128) {
    return take(neverd_web_interface_relation_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
};

TEST_F(WebInterfaceSDK, PreviewIsRequiredAndTokensAreSingleUse) {
  auto P = preview();
  const auto ID = field(P, "capture_id"), T = field(P, "preview_token");
  EXPECT_EQ(field(P, "publication_status"), "preview");
  EXPECT_EQ(code(records(ID)), "har_capture_not_committed");
  EXPECT_EQ(field(preview(11), "status"), "error");
  EXPECT_EQ(code(commit(T)), "invalid_har_preview");
  P = preview();
  const auto Older = field(P, "preview_token");
  const auto Newer = field(preview(), "preview_token");
  EXPECT_NE(Older, Newer);
  EXPECT_EQ(code(commit(Older)), "invalid_har_preview");
  EXPECT_EQ(code(commit(Newer)), "invalid_har_preview");
  P = preview();
  const auto Accepted = field(P, "preview_token");
  const auto H = commit(Accepted);
  EXPECT_EQ(field(H, "publication_status"), "committed");
  EXPECT_EQ(code(commit(Accepted)), "invalid_har_preview");
  EXPECT_EQ(records(ID).getAsObject()->getArray("items")->size(), 1u);
}

TEST_F(WebInterfaceSDK, MetadataAndErrorsNeverEchoSensitiveValues) {
  const auto P = preview();
  EXPECT_EQ(P.getAsObject()->getInteger("text_bodies_withheld"), 2);
  EXPECT_EQ(P.getAsObject()->getInteger("invalid_timestamps"), 1);
  const auto ID = field(commit(field(P, "preview_token")), "capture_id");
  const auto Page = records(ID);
  const auto &O = *(*Page.getAsObject()->getArray("items"))[0].getAsObject();
  EXPECT_EQ(O.getString("method"), "POST");
  EXPECT_EQ(O.getInteger("response_status"), 200);
  EXPECT_EQ(O.getString("response_encoding"), "unsupported_redacted");
  EXPECT_TRUE(O.getBoolean("websocket_extension_unsupported").value_or(false));
  EXPECT_FALSE(
      Page.getAsObject()->getBoolean("capture_authenticated").value_or(true));
  EXPECT_EQ(code(records(ID, 0, 129)), "invalid_page");
  EXPECT_EQ(code(records(ID, 2)), "invalid_page");
  EXPECT_EQ(code(records("CANARY_UNKNOWN")), "har_capture_not_committed");
  EXPECT_EQ(code(take(neverd_web_har_preview_json(
                S, Revision.data(), Revision.size(), nullptr, 64))),
            "invalid_buffer");
  const auto Hash = field(P, "blob_sha256");
  std::ofstream(Root / "a0.har") << "CANARY_REPLACED";
  EXPECT_EQ(field(preview(), "blob_sha256"), Hash);
  const auto OldRevision = Revision;
  open();
  EXPECT_EQ(code(take(neverd_web_har_records_json(S, OldRevision.data(),
                                                  OldRevision.size(), ID.data(),
                                                  ID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(records(ID)), "har_capture_not_committed");
  EXPECT_EQ(field(preview(), "status"), "error");
}

TEST_F(WebInterfaceSDK, CaptureCacheAndImportRevocationAreBounded) {
  std::vector<std::string> IDs;
  for (unsigned I = 1; I <= 4; ++I)
    IDs.push_back(admit(I));
  EXPECT_EQ(code(preview(5)), "har_cache_budget_exceeded");
  EXPECT_EQ(admit(1), IDs[0]);
  const auto Pending = field(preview(1), "preview_token");
  open();
  EXPECT_EQ(code(commit(Pending)), "invalid_har_preview");
  EXPECT_EQ(code(records(IDs[0])), "har_capture_not_committed");
  EXPECT_FALSE(admit(5).empty());
}

TEST_F(WebInterfaceSDK, PreviewCountsURLQueriesAndFormFieldsWithoutTextBodies) {
  std::ofstream(Root / "a0.har") << R"({"log":{"version":"1.2","entries":[{
    "request":{"url":"https://CANARY_HOST/path?token=CANARY&email=CANARY",
    "postData":{"params":[{"name":"password","value":"CANARY"}]}}},
    {"request":{"url":"/relative?token=CANARY"}}]}})";
  open();
  const auto P = preview();
  EXPECT_EQ(P.getAsObject()->getInteger("query_records_withheld"), 0);
  EXPECT_EQ(P.getAsObject()->getInteger("text_bodies_withheld"), 0);
  EXPECT_EQ(P.getAsObject()->getInteger("url_query_components_withheld"), 3);
  EXPECT_EQ(P.getAsObject()->getInteger("url_values_withheld"), 2);
  EXPECT_EQ(P.getAsObject()->getInteger("post_parameter_records_withheld"), 1);
}

#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebInterfaceSDK, StaticAndObservedEvidenceJoinWithoutUpgradingClaims) {
  const auto A = source();
  const auto Analysis = field(A, "interface_analysis_id");
  EXPECT_EQ(A.getAsObject()->getInteger("interface_count"), 1);
  EXPECT_EQ(field(A, "evidence_class"), "static_inference");
  auto P = preview();
  const auto HID = field(P, "capture_id");
  EXPECT_EQ(code(compare(Analysis, HID)), "har_capture_not_committed");
  commit(field(P, "preview_token"));
  const auto C = compare(Analysis, HID);
  EXPECT_EQ(C.getAsObject()->getInteger("pair_count"), 1);
  EXPECT_FALSE(
      C.getAsObject()->getBoolean("source_execution_observed").value_or(true));
  const auto CID = field(C, "correlation_id");
  const auto Page = take(neverd_web_interface_records_json(
      S, Revision.data(), Revision.size(), Analysis.data(), Analysis.size(), 0,
      128));
  const auto &Entry = (*Page.getAsObject()->getArray("items"))[0];
  EXPECT_FALSE(field(Entry, "node_id").empty());
  EXPECT_FALSE(field(Entry, "body_node_id").empty());
  const auto Pairs = take(neverd_web_interface_correlation_records_json(
      S, Revision.data(), Revision.size(), CID.data(), CID.size(), 0, 128));
  const auto &Pair = (*Pairs.getAsObject()->getArray("items"))[0];
  EXPECT_EQ(field(Pair, "interface_id"), field(Entry, "interface_id"));
  const auto Observations = records(HID);
  EXPECT_EQ(field(Pair, "observation_id"),
            field((*Observations.getAsObject()->getArray("items"))[0],
                  "observation_id"));
  EXPECT_EQ(field(Pair, "capture_evidence_class"), "imported_observation");
  open();
  EXPECT_EQ(code(compare(Analysis, HID)), "unknown_interface_analysis");
  EXPECT_EQ(
      code(take(neverd_web_interface_correlation_records_json(
          S, Revision.data(), Revision.size(), CID.data(), CID.size(), 0, 1))),
      "unknown_interface_correlation");
}

TEST_F(WebInterfaceSDK, SourceAndCorrelationCachesAreBounded) {
  std::vector<std::string> Sources, Captures;
  for (unsigned I = 0; I < 4; ++I) {
    Sources.push_back(field(source(6 + I), "interface_analysis_id"));
    Captures.push_back(admit(1 + I));
  }
  EXPECT_EQ(code(source(10)), "interface_cache_budget_exceeded");
  for (const auto &H : Captures)
    EXPECT_EQ(compare(Sources[0], H).getAsObject()->getInteger("pair_count"),
              1);
  EXPECT_EQ(code(compare(Sources[1], Captures[0])),
            "interface_correlation_cache_budget_exceeded");
  EXPECT_EQ(
      compare(Sources[0], Captures[0]).getAsObject()->getInteger("pair_count"),
      1);
}

TEST_F(WebInterfaceSDK,
       WrapperMetadataKeepsCallerAndDefinitionAnchorsRedacted) {
  std::ofstream(Root / "b0.js") << R"(
const CANARY_WRAPPER=(CANARY_OPTIONS,CANARY_URL)=>fetch(CANARY_URL,CANARY_OPTIONS);
CANARY_WRAPPER({method:'POST',headers:{Authorization:'CANARY_TOKEN'},
               body:'CANARY_BODY'},'https://CANARY_HOST/CANARY_PATH');
CANARY_WRAPPER(...CANARY_ARGUMENTS);
)";
  open();
  const auto A = source();
  EXPECT_EQ(field(A, "profile"), "fetch-websocket-xhr-wrappers-syntax-v4");
  EXPECT_EQ(field(A, "wrapper_profile"), "direct-lexical-fetch-forwarding-v1");
  EXPECT_EQ(A.getAsObject()->getInteger("wrapper_definitions"), 1);
  EXPECT_FALSE(
      A.getAsObject()->getBoolean("wrapper_correlation").value_or(true));
  const auto ID = field(A, "interface_analysis_id");
  const auto Page = take(neverd_web_interface_records_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128));
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 3u);
  EXPECT_EQ(field(Items[1], "kind"), "fetch_wrapper_call");
  EXPECT_EQ(field(Items[1], "wrapper_request_node_id"),
            field(Items[0], "node_id"));
  EXPECT_FALSE(field(Items[1], "wrapper_binding_id").empty());
  EXPECT_FALSE(field(Items[1], "wrapper_node_id").empty());
  EXPECT_FALSE(field(Items[1], "url_parameter_node_id").empty());
  EXPECT_FALSE(field(Items[1], "options_parameter_node_id").empty());
  EXPECT_EQ(Items[1].getAsObject()->getInteger("url_argument_index"), 1);
  EXPECT_EQ(Items[1].getAsObject()->getInteger("options_argument_index"), 0);
  EXPECT_EQ(field(Items[1], "method"), "POST");
  EXPECT_FALSE(field(Items[1], "headers_node_id").empty());
  EXPECT_FALSE(field(Items[1], "body_node_id").empty());
  EXPECT_EQ(field(Items[2], "arguments_status"), "spread_unresolved");
  EXPECT_TRUE(field(Items[2], "url_node_id").empty());
  EXPECT_TRUE(field(Items[2], "options_node_id").empty());
  EXPECT_FALSE(Items[2].getAsObject()->getInteger("url_argument_index"));
  EXPECT_FALSE(Items[2].getAsObject()->getInteger("options_argument_index"));
  EXPECT_EQ(field(Items[1], "wrapper_binding_id"),
            field(Items[2], "wrapper_binding_id"));
  EXPECT_EQ(compare(ID, admit()).getAsObject()->getInteger("pair_count"), 0);
  const auto Again = source();
  EXPECT_EQ(field(Again, "interface_analysis_id"), ID);
  const auto OldRevision = Revision;
  open();
  EXPECT_EQ(code(take(neverd_web_interface_records_json(
                S, OldRevision.data(), OldRevision.size(), ID.data(), ID.size(),
                0, 128))),
            "stale_revision");
  EXPECT_EQ(
      code(take(neverd_web_interface_records_json(
          S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128))),
      "unknown_interface_analysis");
}

TEST_F(WebInterfaceSDK, XHRMetadataRetainsSyntaxLinksAndWithholdsAllValues) {
  std::ofstream(Root / "b0.js") << R"(
const CANARY_REQUEST = new XMLHttpRequest();
CANARY_REQUEST.open('POST','https://CANARY_HOST/CANARY_PATH',false,
                    'CANARY_USER','CANARY_PASSWORD');
CANARY_REQUEST.setRequestHeader('Authorization','CANARY_AUTH');
CANARY_REQUEST.setRequestHeader('CANARY_HEADER','CANARY_VALUE');
CANARY_REQUEST.send('CANARY_BODY');
CANARY_REQUEST.onload = () => CANARY_HANDLER(CANARY_REQUEST.responseText);
CANARY_REQUEST.addEventListener('error', CANARY_CALLBACK);
CANARY_REQUEST.send(...CANARY_ARGUMENTS);
)";
  open();
  const auto A = source();
  EXPECT_EQ(field(A, "profile"), "fetch-websocket-xhr-wrappers-syntax-v4");
  EXPECT_EQ(field(A, "receiver_identity"), "lexical_and_member_syntax_only");
  EXPECT_EQ(field(A, "request_ordering"), "not_analyzed");
  EXPECT_FALSE(A.getAsObject()->getBoolean("xhr_correlation").value_or(true));
  const auto ID = field(A, "interface_analysis_id");
  ASSERT_EQ(A.getAsObject()->getInteger("interface_count"), 7);
  const auto Page = take(neverd_web_interface_records_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128));
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 7u);
  const auto Binding = field(Items[0], "receiver_binding_id");
  const auto Constructor = field(Items[0], "constructor_node_id");
  EXPECT_FALSE(Binding.empty());
  EXPECT_FALSE(Constructor.empty());
  for (const auto &I : Items) {
    EXPECT_EQ(field(I, "receiver_binding_id"), Binding);
    EXPECT_EQ(field(I, "constructor_node_id"), Constructor);
    EXPECT_FALSE(field(I, "node_id").empty());
  }
  EXPECT_EQ(field(Items[0], "credential_arguments_status"), "present");
  EXPECT_FALSE(field(Items[0], "async_node_id").empty());
  EXPECT_EQ(field(Items[1], "header_kind"), "sensitive");
  EXPECT_EQ(field(Items[2], "header_kind"), "other");
  EXPECT_FALSE(field(Items[1], "header_value_node_id").empty());
  EXPECT_FALSE(field(Items[3], "body_node_id").empty());
  EXPECT_EQ(field(Items[4], "handler_status"), "function_syntax");
  EXPECT_EQ(field(Items[5], "handler_status"), "value_syntax");
  EXPECT_EQ(field(Items[6], "body_status"), "spread_unresolved");
  EXPECT_TRUE(field(Items[6], "body_node_id").empty());
  EXPECT_EQ(compare(ID, admit()).getAsObject()->getInteger("pair_count"), 0);
}

TEST_F(WebInterfaceSDK, MemberAndSpreadPublicationKeepExplicitUncertainty) {
  std::ofstream(Root / "b0.js") << R"(
const CANARY_CLIENT = obtainClient();
CANARY_CLIENT.fetch('https://CANARY_HOST/CANARY_PATH', {method:'POST',body:'CANARY_BODY'});
this.fetch(...CANARY_ARGUMENTS);
fetch(...CANARY_ARGUMENTS);
new WebSocket(...CANARY_ARGUMENTS);
fetch('https://CANARY_HOST/CANARY_PATH', {method:'POST'});
)";
  open();
  const auto A = source();
  EXPECT_EQ(field(A, "profile"), "fetch-websocket-xhr-wrappers-syntax-v4");
  EXPECT_EQ(field(A, "fetch_member_identity"), "property_spelling_unverified");
  EXPECT_FALSE(
      A.getAsObject()->getBoolean("fetch_member_correlation").value_or(true));
  EXPECT_EQ(field(A, "spread_argument_resolution"), "not_analyzed");
  const auto ID = field(A, "interface_analysis_id");
  const auto Page = take(neverd_web_interface_records_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128));
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 5u);
  EXPECT_EQ(field(Items[0], "kind"), "fetch_member_call");
  EXPECT_FALSE(field(Items[0], "receiver_node_id").empty());
  EXPECT_FALSE(field(Items[0], "receiver_binding_id").empty());
  EXPECT_EQ(field(Items[0], "method"), "unknown");
  EXPECT_EQ(field(Items[0], "url_status"), "wrapper_arguments_not_interpreted");
  EXPECT_TRUE(field(Items[0], "url_node_id").empty());
  EXPECT_TRUE(field(Items[0], "options_node_id").empty());
  EXPECT_TRUE(field(Items[0], "body_node_id").empty());
  for (size_t I = 1; I < 4; ++I) {
    EXPECT_EQ(field(Items[I], "arguments_status"), "spread_unresolved");
    EXPECT_EQ(field(Items[I], "method"), "unknown");
    for (const auto *Name : {"url_node_id", "options_node_id", "method_node_id",
                             "headers_node_id", "body_node_id"})
      EXPECT_TRUE(field(Items[I], Name).empty());
  }
  const auto C = compare(ID, admit());
  EXPECT_EQ(C.getAsObject()->getInteger("pair_count"), 1);
  const auto CID = field(C, "correlation_id");
  const auto Pairs = take(neverd_web_interface_correlation_records_json(
      S, Revision.data(), Revision.size(), CID.data(), CID.size(), 0, 128));
  const auto &Matched = *Pairs.getAsObject()->getArray("items");
  ASSERT_EQ(Matched.size(), 1u);
  EXPECT_EQ(field(Matched[0], "interface_id"), field(Items[4], "interface_id"));
}
#endif

#if NEVERD_TEST_WEB_JAVASCRIPT
TEST_F(WebInterfaceSDK,
       RequestRelationsKeepPrivateValuesAndRuntimeClaimsSeparate) {
  std::ofstream(Root / "b0.js", std::ios::trunc) << R"(
async function CANARY_WRAPPER() {
  const CANARY_RESULT = await fetch('https://CANARY_HOST/CANARY_PATH',
      {method:'POST',body:'CANARY_BODY'});
  CANARY_RESULT.json();
  return fetch(...CANARY_ARGUMENTS);
}
fetch(...CANARY_ARGUMENTS).then(CANARY_HANDLER);
)";
  open();
  const auto Request = source();
  const auto ID = field(Request, "interface_analysis_id");
  const auto A = relations(ID);
  EXPECT_EQ(field(A, "profile"), "request-source-relations-v2");
  EXPECT_EQ(field(A, "analysis_status"), "partial");
  EXPECT_EQ(field(A, "response_api_identity"), "member_spelling_unverified");
  EXPECT_EQ(field(A, "wrapper_argument_flow"), "not_analyzed");
  EXPECT_EQ(field(A, "callback_parameter_roles"), "not_assigned");
  EXPECT_FALSE(
      A.getAsObject()->getBoolean("source_execution_observed").value_or(true));
  EXPECT_FALSE(A.getAsObject()->getBoolean("har_correlation").value_or(true));
  EXPECT_EQ(A.getAsObject()->getInteger("relation_count"), 6);
  const auto RID = field(A, "relations_id");
  const auto Page = relationRecords(RID, 0, 2);
  ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 2u);
  EXPECT_EQ(Page.getAsObject()->getInteger("next_offset"), 2);
  EXPECT_EQ(relationRecords(RID, 6).getAsObject()->getArray("items")->size(),
            0u);
  EXPECT_EQ(code(relationRecords(RID, 7)), "invalid_page");
  EXPECT_EQ(code(relationRecords(RID, 0, 129)), "invalid_page");
  EXPECT_EQ(code(relations("unknown")), "unknown_interface_analysis");
  const auto H = admit();
  const auto Joined = compare(ID, H);
  EXPECT_EQ(Joined.getAsObject()->getInteger("pair_count"), 1);
  EXPECT_EQ(field(relations(ID), "relations_id"), RID);
  const auto OldRevision = Revision;
  open();
  EXPECT_EQ(code(take(neverd_web_interface_relation_records_json(
                S, OldRevision.data(), OldRevision.size(), RID.data(),
                RID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(relationRecords(RID)), "unknown_interface_relations");
}

TEST_F(WebInterfaceSDK,
       LocalAndPublishedNavigationGiveIdenticalRequestRelations) {
  std::ofstream(Root / "b0.js", std::ios::trunc)
      << "const CANARY_RESULT = await fetch(...CANARY_ARGS); "
         "CANARY_RESULT.text();";
  open();
  auto Request = source();
  const auto Before = relations(field(Request, "interface_analysis_id"));
  const auto BeforeID = field(Before, "relations_id");
  const auto BeforeSteps = Before.getAsObject()->getInteger("steps");
  const auto BeforeItems =
      relationRecords(BeforeID).getAsObject()->getArray("items")->size();
  open();
  Request = source();
  const auto SourceID = field(Request, "source_id");
  const auto Navigation = take(neverd_web_source_navigation_analyze_json(
      S, Revision.data(), Revision.size(), SourceID.data(), SourceID.size()));
  EXPECT_EQ(field(Navigation, "navigation_id"), field(Before, "navigation_id"));
  const auto After = relations(field(Request, "interface_analysis_id"));
  EXPECT_EQ(field(After, "relations_id"), BeforeID);
  EXPECT_EQ(After.getAsObject()->getInteger("steps"), BeforeSteps);
  EXPECT_EQ(relationRecords(BeforeID).getAsObject()->getArray("items")->size(),
            BeforeItems);
}

TEST_F(WebInterfaceSDK, RelationRecordExhaustionKeepsRequestPagesAvailable) {
  {
    std::ofstream O(Root / "b0.js", std::ios::trunc);
    for (unsigned I = 0; I < 2050; ++I)
      O << "const CANARY_RESULT_" << I << " = await fetch(url); CANARY_RESULT_"
        << I << ".json();\n";
  }
  open();
  const auto Request = source();
  ASSERT_EQ(field(Request, "analysis_status"), "partial");
  ASSERT_EQ(Request.getAsObject()->getInteger("interface_count"), 2050);
  const auto ID = field(Request, "interface_analysis_id");
  const auto A = relations(ID);
  EXPECT_EQ(field(A, "analysis_status"), "budget_exceeded");
  EXPECT_EQ(field(A, "reason"), "interface_relation_record_budget_exceeded");
  EXPECT_EQ(A.getAsObject()->getInteger("relation_count"), 0);
  const auto RID = field(A, "relations_id");
  EXPECT_TRUE(relationRecords(RID).getAsObject()->getArray("items")->empty());
  const auto Page = take(neverd_web_interface_records_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128));
  EXPECT_EQ(Page.getAsObject()->getArray("items")->size(), 128u);
  EXPECT_EQ(field(Page, "analysis_status"), "partial");
}
#endif

TEST_F(WebInterfaceSDK, PassiveReportsRequireCommitAndKeepEvidenceClasses) {
  auto Create = [&](std::string Text) {
    return take(neverd_web_passive_report_create_json(
        S, Revision.data(), Revision.size(), Text.data(), Text.size()));
  };
  auto Read = [&](std::string ID, uint64_t At = 0, uint64_t Limit = 128) {
    return take(neverd_web_passive_report_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), At, Limit));
  };
  const auto P = preview();
  const auto Capture = field(P, "capture_id");
  const auto Selection =
      "{\"schema_version\":1,\"har_capture_id\":\"" + Capture + "\"}";
  EXPECT_EQ(code(Create(Selection)), "har_capture_not_committed");
  EXPECT_EQ(code(Create("{\"schema_version\":1}")),
            "passive_report_empty_selection");
  EXPECT_EQ(code(Create("{\"schema_version\":1,\"raw\":true}")),
            "invalid_passive_report_selection");
#if !NEVERD_TEST_WEB_JAVASCRIPT
  EXPECT_EQ(
      code(Create("{\"schema_version\":1,\"interface_analysis_id\":\"a\"}")),
      "capability_unavailable");
#endif
  commit(field(P, "preview_token"));
  const auto R = Create(Selection);
  const auto ID = field(R, "passive_report_id");
  ASSERT_FALSE(ID.empty());
  EXPECT_EQ(field(Create(Selection), "passive_report_id"), ID);
  EXPECT_EQ(R.getAsObject()->getInteger("record_count"), 3);
  EXPECT_EQ(R.getAsObject()->getInteger("static_records"), 0);
  EXPECT_EQ(field(R, "server_constraints"), "unknown");
  EXPECT_EQ(field(R, "inferred_optionality"), "unknown");
  EXPECT_EQ(code(Read(ID, 0, 129)), "invalid_page");
  EXPECT_FALSE(
      Read(ID, 0, 1).getAsObject()->getBoolean("page_complete").value_or(true));
  EXPECT_TRUE(Read(ID, 2, 1)
                  .getAsObject()
                  ->getBoolean("page_complete")
                  .value_or(false));
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Analysis = field(source(), "interface_analysis_id");
  const auto Both =
      Create("{\"schema_version\":1,\"har_capture_id\":\"" + Capture +
             "\",\"interface_analysis_id\":\"" + Analysis + "\"}");
  EXPECT_EQ(Both.getAsObject()->getInteger("http_candidates"), 1);
  const auto Pages = Read(field(Both, "passive_report_id"));
  std::set<std::string> Classes;
  for (const auto &Item : *Pages.getAsObject()->getArray("items"))
    Classes.insert(field(Item, "evidence_class"));
  EXPECT_EQ(Classes,
            (std::set<std::string>{"static_inference", "imported_observation",
                                   "candidate_correlation"}));
#endif
  open();
  EXPECT_EQ(code(Read(ID)), "unknown_passive_report");
}

TEST_F(WebInterfaceSDK, PassiveStructureRetainsFieldsAcrossSingleRowPages) {
  std::ofstream(Root / "a0.har") << R"({"log":{"version":"1.2","entries":[{
        "request":{"method":"POST","url":"https://CANARY/a",
          "postData":{"mimeType":"application/json",
                      "text":"{\"CANARY_PARENT\":{\"CANARY_CHILD\":null}}"}},
        "response":{"status":200}}]}})";
  open();
  const auto Capture = admit();
  const auto Selection =
      "{\"schema_version\":1,\"har_capture_id\":\"" + Capture + "\"}";
  const auto Report = take(neverd_web_passive_report_create_json(
      S, Revision.data(), Revision.size(), Selection.data(), Selection.size()));
  ASSERT_EQ(field(Report, "status"), "ok");
  EXPECT_EQ(field(Report, "profile"), "passive-evidence-shape-report-v2");
  EXPECT_EQ(field(Report, "schema_meaning"), "observed_structure_and_presence");
  const auto ID = field(Report, "passive_report_id");
  std::set<std::string> Nodes, Parents, IDs;
  uint64_t Values = 0, Schemas = 0, Rows = 0;
  for (;;) {
    const auto Page = take(neverd_web_passive_report_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Rows, 1));
    ASSERT_EQ(field(Page, "status"), "ok");
    const auto *Items = Page.getAsObject()->getArray("items");
    ASSERT_NE(Items, nullptr);
    ASSERT_EQ(Items->size(), 1u);
    const auto &R = (*Items)[0];
    EXPECT_TRUE(IDs.insert(field(R, "record_id")).second);
    Values += field(R, "kind") == "observed_json_value_node";
    Schemas += field(R, "kind") == "observed_json_schema_node";
    if (const auto *S = R.getAsObject()->getObject("structure")) {
      const auto Node = S->getString("node_id").value_or("").str();
      const auto Parent = S->getString("parent_node_id").value_or("").str();
      if (!Node.empty())
        EXPECT_TRUE(Nodes.insert(Node).second);
      if (!Parent.empty())
        Parents.insert(Parent);
    }
    ++Rows;
    if (Page.getAsObject()->getBoolean("page_complete").value_or(false))
      break;
    ASSERT_LT(Rows, 20u);
  }
  EXPECT_EQ(Values, 3u);
  EXPECT_EQ(Schemas, 3u);
  EXPECT_EQ(Rows, 9u);
  for (const auto &P : Parents)
    EXPECT_TRUE(Nodes.count(P));
}

TEST_F(WebInterfaceSDK,
       PassiveStreamSelectionPreservesSourceAndProtocolLimits) {
  std::ofstream(Root / "bad.har")
      << R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"CANARY":"CANARY"}})"
      << '\n';
  std::ofstream(Root / "b0.js")
      << "function f(){checkPermission(CANARY);executeTool(CANARY);"
         "runHooks(CANARY);const c={hooks:{},mcpServers:{},permissions:{}};"
         "send({jsonrpc:'2.0',method:'tools/call',params:{CANARY:'CANARY'}});}";
  open();
  const auto &Artifact = Artifacts[11];
  const std::string Profile = "mcp-2025-06-18-stdio-jsonl-v1";
  const auto Preview = take(neverd_web_stream_preview_json(
      S, Revision.data(), Revision.size(), Artifact.data(), Artifact.size(),
      Profile.data(), Profile.size()));
  const auto Stream = field(Preview, "stream_capture_id");
  auto Create = [&](std::string Text) {
    return take(neverd_web_passive_report_create_json(
        S, Revision.data(), Revision.size(), Text.data(), Text.size()));
  };
  auto Selection =
      "{\"schema_version\":1,\"stream_capture_id\":\"" + Stream + "\"}";
  EXPECT_EQ(code(Create(Selection)), "stream_capture_not_committed");
  const auto Token = field(Preview, "preview_token");
  take(neverd_web_stream_commit_json(S, Revision.data(), Revision.size(),
                                     Token.data(), Token.size()));
  EXPECT_EQ(
      Create(Selection).getAsObject()->getInteger("mcp_method_candidates"), 0);
#if NEVERD_TEST_WEB_JAVASCRIPT
  const auto Analysis = field(source(), "interface_analysis_id");
  const auto R =
      Create("{\"schema_version\":1,\"stream_capture_id\":\"" + Stream +
             "\",\"interface_analysis_id\":\"" + Analysis + "\"}");
  EXPECT_EQ(R.getAsObject()->getInteger("mcp_method_candidates"), 1);
  const auto ID = field(R, "passive_report_id");
  const auto Page = take(neverd_web_passive_report_records_json(
      S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128));
  EXPECT_GT(Page.getAsObject()->getArray("items")->size(), 5u);
  EXPECT_EQ(Page.getAsObject()->getBoolean("permission_enforcement_verified"),
            false);
#endif
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebInterfaceSDK, CLIRequiresMatchingPreviewHashAndPolicyWithoutTools) {
  const auto Path = Root.string(), Out = Path + ".stdout",
             Err = Path + ".stderr";
  const llvm::StringRef Env[] = {"PATH=/neverd-no-external-tools", "LC_ALL=C"};
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  std::string Output, Errors;
  auto Run = [&](std::vector<std::string> Arguments) {
    // ExecuteAndWait redirects may preserve an earlier, longer file tail.
    std::ofstream(Out, std::ios::trunc).close();
    std::ofstream(Err, std::ios::trunc).close();
    llvm::SmallVector<llvm::StringRef> Args{NEVERD_WEB_TEST_CLI, "web"};
    for (const auto &A : Arguments)
      Args.push_back(A);
    const auto Status = llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args,
                                                  Env, Redirects, 30);
    std::ifstream O(Out), E(Err);
    Output.assign(std::istreambuf_iterator<char>(O), {});
    Errors.assign(std::istreambuf_iterator<char>(E), {});
    redacted(Output);
    redacted(Errors);
    return Status;
  };
  ASSERT_EQ(Run({"har-preview", Path, "1"}), 0);
  const auto P = parse(Output);
  const auto Policy = field(P, "redaction_policy");
  const auto Hash = field(P, "blob_sha256");
  EXPECT_EQ(Run({"har-import", Path, "1"}), 2);
  EXPECT_EQ(Run({"har-import", Path, "1", Policy, std::string(64, '0')}), 1);
  EXPECT_EQ(Output.find("observation_id"), std::string::npos);
  ASSERT_EQ(Run({"har-import", Path, "1", Policy, Hash}), 0);
  EXPECT_NE(Output.find("observation_id"), std::string::npos);
  ASSERT_EQ(Run({"report-har", Path, "1", Policy, Hash}), 0);
  EXPECT_NE(Output.find("passive-evidence-shape-report-v2"), std::string::npos);
#if NEVERD_TEST_WEB_JAVASCRIPT
  std::ofstream(Root / "b0.js", std::ios::app)
      << "const CANARY_XHR=new XMLHttpRequest();"
         "CANARY_XHR.open('POST','https://CANARY_HOST/CANARY_PATH');"
         "CANARY_XHR.send('CANARY_BODY');"
         "CANARY_XHR.onload=CANARY_CALLBACK;";
  EXPECT_EQ(Run({"interfaces", Path, "module", "6"}), 0);
  EXPECT_NE(Output.find("\"evidence_class\":\"static_inference\""),
            std::string::npos);
  EXPECT_NE(Output.find("\"kind\":\"xhr_open\""), std::string::npos);
  EXPECT_NE(Output.find("\"kind\":\"xhr_send\""), std::string::npos);
  EXPECT_NE(Output.find("\"kind\":\"xhr_event_handler\""), std::string::npos);
  std::ofstream(Root / "b0.js", std::ios::app)
      << "const CANARY_RESULT=await "
         "fetch(...CANARY_ARGS);CANARY_RESULT.json();";
  EXPECT_EQ(Run({"interface-relations", Path, "module", "6"}), 0);
  EXPECT_NE(Output.find("request-source-relations-v2"), std::string::npos);
  EXPECT_NE(Output.find("await_result_const_binding"), std::string::npos);
  EXPECT_NE(Output.find("bound_result_member_call"), std::string::npos);
  EXPECT_NE(
      Output.find("\"response_api_identity\":\"member_spelling_unverified\""),
      std::string::npos);
  EXPECT_EQ(
      Run({"interface-correlate", Path, "module", "6", "1", Policy, Hash}), 0);
  EXPECT_NE(Output.find("\"pair_count\":1"), std::string::npos);
  EXPECT_EQ(Run({"report-source", Path, "module", "6"}), 0);
  EXPECT_NE(Output.find("source_interface"), std::string::npos);
  EXPECT_EQ(Run({"report-correlate", Path, "module", "6", "1", Policy, Hash}),
            0);
  EXPECT_NE(Output.find("http_source_observation_candidate"),
            std::string::npos);
#endif
  std::ofstream(Root / "a0.har", std::ios::app) << " ";
  EXPECT_EQ(Run({"har-import", Path, "1", Policy, Hash}), 1);
  EXPECT_NE(Errors.find("har_preview_receipt_mismatch"), std::string::npos);
}
#endif
} // namespace

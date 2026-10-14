//===- BranchSDKTests.cpp - Boolean branch API and local export ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test profile evidence, private source, shared cache and CLI parity.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <array>
#include <filesystem>
#include <fstream>

namespace {
using Value = llvm::json::Value;

Value parse(std::string_view Text) {
  EXPECT_EQ(Text.find("CANARY"), std::string_view::npos);
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

std::string field(const Value &V, const char *Key) {
  return V.getAsObject()->getString(Key).value_or("").str();
}

std::string code(const Value &V) {
  EXPECT_EQ(field(V, "status"), "error");
  const auto *E = V.getAsObject()->getObject("error");
  return E ? E->getString("code").value_or("").str() : "";
}

class WebBranchSDK : public ::testing::Test {
protected:
  static constexpr std::string_view Original =
      "/*CANARY_LICENSE*/function CANARY_NAME(CANARY_ARG){"
      "return true ? CANARY_ARG.method() : CANARY_UNUSED;}"
      "const x=false ? 'CANARY_SECRET' : kept;";
  static constexpr std::string_view Candidate =
      "/*CANARY_LICENSE*/function CANARY_NAME(CANARY_ARG){"
      "return (0,(CANARY_ARG.method()));}const x=(0,(kept));";
  std::filesystem::path Root, Input;
  neverd_web_session_t S = nullptr;
  std::string Revision, Source;

  void SetUp() override {
    if (!NEVERD_TEST_WEB_JAVASCRIPT)
      GTEST_SKIP() << "JavaScript parser omitted";
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-branch-sdk", Path));
    Root = Path.str().str();
    Input = Root / "input.js";
    std::ofstream(Input) << Original;
    S = neverd_web_session_create();
    ASSERT_NE(S, nullptr);
    open();
  }

  void TearDown() override {
    neverd_web_session_destroy(S);
    if (!Root.empty()) {
      std::error_code EC;
      std::filesystem::remove_all(Root, EC);
    }
  }

  void open() {
    const auto Path = Input.string();
    const auto P = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(P, "preview_token");
    Revision = field(
        take(neverd_web_import_commit_json(S, Token.data(), Token.size())),
        "revision");
    const auto Page = take(
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 1));
    const auto Artifact =
        field(Page.getAsObject()->getArray("items")->front(), "artifact_id");
    Source = field(take(neverd_web_source_analyze_json(
                       S, Revision.data(), Revision.size(), Artifact.data(),
                       Artifact.size(), "script", 6)),
                   "source_id");
  }

  Value analyze(std::string_view Pass = "boolean_branches") {
    return take(neverd_web_source_rewrite_analyze_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size(),
        Pass.data(), Pass.size()));
  }

  Value records(std::string_view ID) {
    return take(neverd_web_source_rewrite_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), 0, 128));
  }

  Value exportTo(const Value &A, std::string_view Receipt,
                 const std::filesystem::path &Directory) {
    const auto ID = field(A, "rewrite_id"), Out = Directory.string();
    return take(neverd_web_source_rewrite_export_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Receipt.data(), Receipt.size(), Out.data(), Out.size()));
  }

  static std::string read(const std::filesystem::path &Path) {
    std::ifstream F(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(F), {}};
  }
};

TEST_F(WebBranchSDK,
       DistinctBranchEvidenceDoesNotExposeOperandsOrAssumePurity) {
  const auto A = analyze();
  ASSERT_EQ(field(A, "analysis_status"), "verified_local_rewrites");
  EXPECT_EQ(field(A, "profile"), "javascript-boolean-branches-v1");
  EXPECT_EQ(field(A, "value_verification"), "constant_boolean_condition_only");
  EXPECT_EQ(field(A, "binding_verification"),
            "same_scopes_declarations_and_retained_references");
  EXPECT_EQ(field(A, "retained_expression_effects"),
            "preserved_not_assumed_absent");
  EXPECT_EQ(A.getAsObject()->getBoolean("statements_rewritten"), false);
  EXPECT_EQ(A.getAsObject()->getInteger("accepted_count"), 2);
  EXPECT_FALSE(field(A, "value_analysis_id").empty());
  const auto Page = records(field(A, "rewrite_id"));
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 2U);
  for (const auto &R : Items) {
    EXPECT_FALSE(field(R, "condition_node_id").empty());
    EXPECT_FALSE(field(R, "retained_node_id").empty());
    EXPECT_FALSE(field(R, "selected_operand").empty());
    for (const auto *Key : {"binding_id", "identifier_role", "before", "after",
                            "before_sha256", "after_sha256"})
      EXPECT_FALSE(R.getAsObject()->get(Key));
  }
  std::ofstream(Input, std::ios::trunc) << "CANARY_CHANGED";
  const auto E = exportTo(A, field(A, "export_receipt"), Root / "export");
  EXPECT_EQ(field(E, "export_status"), "verified_local_files");
  EXPECT_EQ(read(Root / "export" / "original.js"), Original);
  EXPECT_EQ(read(Root / "export" / "candidate.js"), Candidate);
  const auto Receipt = parse(read(Root / "export" / "receipt.json"));
  EXPECT_EQ(field(Receipt, "inverse_verification"), "exact_original_sha256");
  EXPECT_EQ(read(Input), "CANARY_CHANGED");
}

TEST_F(WebBranchSDK, CacheAndReceiptAuthorityAreSharedAcrossProfiles) {
  const auto A = analyze();
  const auto R = analyze("scope_renaming");
  EXPECT_NE(field(A, "rewrite_id"), field(R, "rewrite_id"));
  EXPECT_EQ(code(analyze("constant_folding")),
            "source_rewrite_cache_budget_exceeded");
  EXPECT_EQ(field(analyze(), "rewrite_id"), field(A, "rewrite_id"));
  EXPECT_EQ(code(exportTo(A, field(R, "export_receipt"), Root / "wrong")),
            "source_rewrite_export_receipt_mismatch");
  EXPECT_FALSE(std::filesystem::exists(Root / "wrong"));
  const auto Previous = Revision;
  open();
  EXPECT_NE(Revision, Previous);
  EXPECT_EQ(code(records(field(A, "rewrite_id"))), "unknown_source_rewrite");
  const auto Renewed = analyze();
  EXPECT_EQ(field(A, "rewrite_id"), field(Renewed, "rewrite_id"));
  EXPECT_EQ(code(exportTo(Renewed, field(A, "export_receipt"), Root / "stale")),
            "source_rewrite_export_receipt_mismatch");
}

TEST_F(WebBranchSDK, CLIExportsTheSameCandidateWithoutAnExternalRuntime) {
#ifndef NEVERD_WEB_TEST_CLI
  GTEST_SKIP() << "CLI unavailable";
#else
  const auto Out = (Root / "stdout").string(), Err = (Root / "stderr").string();
  auto Run = [&](std::vector<std::string> Args) {
    Args.insert(Args.begin(), {NEVERD_WEB_TEST_CLI, "web"});
    std::vector<llvm::StringRef> Refs;
    for (const auto &A : Args)
      Refs.emplace_back(A);
    std::array<std::optional<llvm::StringRef>, 3> Redirects{std::nullopt, Out,
                                                            Err};
    std::array<llvm::StringRef, 1> Env{"PATH=/neverd-no-external-tools"};
    std::ofstream(Out, std::ios::trunc).close();
    std::ofstream(Err, std::ios::trunc).close();
    EXPECT_EQ(llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Refs,
                                        llvm::ArrayRef(Env), Redirects, 60),
              0)
        << read(Err);
    std::ifstream Lines(Out);
    std::vector<Value> Results;
    for (std::string Line; std::getline(Lines, Line);)
      if (!Line.empty())
        Results.push_back(parse(Line));
    return Results;
  };
  const auto Preview =
      Run({"rewrite", Input.string(), "script", "boolean_branches"});
  ASSERT_FALSE(Preview.empty());
  const auto A = analyze();
  EXPECT_EQ(field(Preview.front(), "rewrite_id"), field(A, "rewrite_id"));
  EXPECT_EQ(field(Preview.front(), "export_receipt"),
            field(A, "export_receipt"));
  const auto Result =
      Run({"rewrite-export", Input.string(), "script", "boolean_branches", "0",
           field(A, "export_receipt"), (Root / "cli").string()});
  ASSERT_EQ(Result.size(), 2U);
  EXPECT_EQ(field(Result.back(), "export_status"), "verified_local_files");
  EXPECT_EQ(read(Root / "cli" / "candidate.js"), Candidate);
  EXPECT_EQ(read(Root / "cli" / "original.js"), Original);
#endif
}
} // namespace

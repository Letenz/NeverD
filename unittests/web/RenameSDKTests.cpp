//===- RenameSDKTests.cpp - Lexical rename SDK and CLI evidence -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify private names, profile-specific receipts and explicit local export.
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
  EXPECT_EQ(Text.find("neverd_arg_"), std::string_view::npos);
  EXPECT_EQ(Text.find("neverd_local_"), std::string_view::npos);
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

class WebRenameSDK : public ::testing::Test {
protected:
  static constexpr std::string_view Original =
      "/*CANARY_LICENSE*/function CANARY_NAME(CANARY_ARG){"
      "const CANARY_LOCAL=CANARY_ARG;return CANARY_LOCAL;}";
  static constexpr std::string_view Candidate =
      "/*CANARY_LICENSE*/function CANARY_NAME(neverd_arg_2){"
      "const neverd_local_3=neverd_arg_2;return neverd_local_3;}";
  std::filesystem::path Root, Input;
  neverd_web_session_t S = nullptr;
  std::string Revision, Source;

  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX capture profile required";
#endif
    if (!NEVERD_TEST_WEB_JAVASCRIPT)
      GTEST_SKIP() << "JavaScript parser omitted";
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-rename-sdk", Path));
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

  Value analyze(std::string_view Pass = "scope_renaming") {
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

TEST_F(WebRenameSDK, DistinctLexicalEvidenceKeepsNamesPrivateUntilLocalExport) {
  const auto A = analyze();
  ASSERT_EQ(field(A, "analysis_status"), "verified_local_rewrites");
  EXPECT_EQ(field(A, "profile"), "javascript-scope-rename-v1");
  EXPECT_EQ(field(A, "value_verification"), "not_applicable");
  EXPECT_EQ(field(A, "binding_verification"),
            "same_scopes_declarations_and_reference_resolution");
  EXPECT_EQ(A.getAsObject()->getInteger("accepted_count"), 4);
  EXPECT_EQ(A.getAsObject()->getBoolean("original_name_recovery"), false);
  for (const auto *Key : {"value_analysis_id", "effect_analysis_id",
                          "value_profile", "effect_profile"}) {
    const auto *V = A.getAsObject()->get(Key);
    ASSERT_NE(V, nullptr);
    EXPECT_EQ(V->kind(), Value::Null);
  }
  const auto Page = records(field(A, "rewrite_id"));
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 5U);
  for (const auto &R : Items) {
    EXPECT_FALSE(field(R, "binding_id").empty());
    EXPECT_FALSE(field(R, "identifier_role").empty());
    EXPECT_FALSE(R.getAsObject()->get("before"));
    EXPECT_FALSE(R.getAsObject()->get("after"));
    EXPECT_FALSE(R.getAsObject()->get("before_sha256"));
    EXPECT_FALSE(R.getAsObject()->get("after_sha256"));
  }
  std::ofstream(Input, std::ios::trunc) << "CANARY_CHANGED";
  const auto Out = Root / "export";
  const auto E = exportTo(A, field(A, "export_receipt"), Out);
  EXPECT_EQ(field(E, "export_status"), "verified_local_files");
  EXPECT_EQ(read(Out / "original.js"), Original);
  EXPECT_EQ(read(Out / "candidate.js"), Candidate);
  const auto Receipt = parse(read(Out / "receipt.json"));
  EXPECT_EQ(field(Receipt, "profile"), "javascript-scope-rename-v1");
  EXPECT_EQ(field(Receipt, "inverse_verification"), "exact_original_sha256");
  EXPECT_EQ(read(Input), "CANARY_CHANGED");
}

TEST_F(WebRenameSDK, SharedCacheAndRevisionBoundReceiptsRemainAuthoritative) {
  const auto A = analyze();
  const auto P = analyze("constant_folding");
  EXPECT_NE(field(A, "rewrite_id"), field(P, "rewrite_id"));
  EXPECT_EQ(code(analyze("string_recovery")),
            "source_rewrite_cache_budget_exceeded");
  EXPECT_EQ(field(analyze(), "rewrite_id"), field(A, "rewrite_id"));
  EXPECT_EQ(code(exportTo(A, field(P, "export_receipt"), Root / "wrong")),
            "source_rewrite_export_receipt_mismatch");
  EXPECT_FALSE(std::filesystem::exists(Root / "wrong"));
  const auto Previous = Revision;
  open();
  EXPECT_NE(Revision, Previous);
  EXPECT_EQ(code(records(field(A, "rewrite_id"))), "unknown_source_rewrite");
  const auto Renewed = analyze();
  EXPECT_EQ(field(Renewed, "rewrite_id"), field(A, "rewrite_id"));
  EXPECT_NE(field(Renewed, "export_receipt"), field(A, "export_receipt"));
  EXPECT_EQ(code(exportTo(Renewed, field(A, "export_receipt"), Root / "stale")),
            "source_rewrite_export_receipt_mismatch");
}

TEST_F(WebRenameSDK, RenameV1ReceiptRemainsExactlyCompatible) {
  const auto GoldenBytes = read(std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) /
                                "rewrite" / "scope-renaming-v1-receipt.json");
  ASSERT_FALSE(GoldenBytes.empty());
  const auto Golden = parse(GoldenBytes);
  const auto A = analyze();
  EXPECT_EQ(field(A, "rewrite_id"), field(Golden, "rewrite_id"));
  EXPECT_EQ(field(A, "export_receipt"), field(Golden, "export_receipt"));
  const auto E = exportTo(A, field(A, "export_receipt"), Root / "golden");
  EXPECT_EQ(field(E, "receipt_sha256"),
            "d2c8b50fdec7fa921b1d771bb8726d6aaa8945e24174b56a9a8752a324311e1b");
  EXPECT_EQ(read(Root / "golden" / "receipt.json"), GoldenBytes);
}

TEST_F(WebRenameSDK, CLIUsesTheSameReceiptWithNoExternalRuntime) {
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
    // Reused redirect files can retain the tail of an earlier, longer reply.
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
      Run({"rewrite", Input.string(), "script", "scope_renaming"});
  ASSERT_FALSE(Preview.empty());
  const auto A = analyze();
  EXPECT_EQ(field(Preview.front(), "rewrite_id"), field(A, "rewrite_id"));
  EXPECT_EQ(field(Preview.front(), "export_receipt"),
            field(A, "export_receipt"));
  const auto Result =
      Run({"rewrite-export", Input.string(), "script", "scope_renaming", "0",
           field(A, "export_receipt"), (Root / "cli-export").string()});
  ASSERT_EQ(Result.size(), 2U);
  EXPECT_EQ(field(Result.back(), "export_status"), "verified_local_files");
  EXPECT_EQ(read(Root / "cli-export" / "original.js"), Original);
  EXPECT_EQ(read(Root / "cli-export" / "candidate.js"), Candidate);
#endif
}
} // namespace

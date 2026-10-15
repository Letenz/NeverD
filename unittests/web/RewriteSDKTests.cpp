//===- RewriteSDKTests.cpp - Rewrite publication and local CLI contracts --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata privacy, pass-bound export receipts and immutable session sources.
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

class WebRewriteSDK : public ::testing::Test {
protected:
  static constexpr std::string_view Original =
      "/*CANARY_LICENSE*/const a='CAN'+'ARY_RECOVERED';const b=1+2;";
  std::filesystem::path Root, Input;
  neverd_web_session_t S = nullptr;
  std::string Revision;
  std::vector<std::string> Artifacts;
  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-rewrite-sdk", Path));
    Root = Path.str().str();
    Input = Root / "input";
    std::filesystem::create_directory(Input);
    std::ofstream(Input / "a.js") << Original;
    std::ofstream(Input / "b.js") << "const b=1+3;";
    std::ofstream(Input / "c.js") << "const c=1+4;";
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
        neverd_web_artifacts_json(S, Revision.data(), Revision.size(), 0, 128));
    Artifacts.clear();
    for (const auto &I : *Page.getAsObject()->getArray("items"))
      Artifacts.push_back(field(I, "artifact_id"));
    // Directory capture includes the root before its three sorted members.
    ASSERT_EQ(Artifacts.size(), 4u);
  }
  std::string source(unsigned Index = 1) {
    const auto &A = Artifacts.at(Index);
    return field(
        take(neverd_web_source_analyze_json(S, Revision.data(), Revision.size(),
                                            A.data(), A.size(), "script", 6)),
        "source_id");
  }
  Value analyze(std::string_view Source, std::string_view Pass) {
    return take(neverd_web_source_rewrite_analyze_json(
        S, Revision.data(), Revision.size(), Source.data(), Source.size(),
        Pass.data(), Pass.size()));
  }
  Value records(std::string_view ID, uint64_t Offset = 0,
                uint64_t Limit = 128) {
    return take(neverd_web_source_rewrite_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Offset,
        Limit));
  }
  Value exportTo(const Value &A, std::string_view Receipt,
                 const std::filesystem::path &Path) {
    const auto ID = field(A, "rewrite_id"), Out = Path.string();
    return take(neverd_web_source_rewrite_export_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(),
        Receipt.data(), Receipt.size(), Out.data(), Out.size()));
  }
  static std::string read(const std::filesystem::path &Path) {
    std::ifstream F(Path, std::ios::binary);
    return {std::istreambuf_iterator<char>(F), {}};
  }
};

TEST_F(WebRewriteSDK, MetadataKeepsRecoveredValuesPrivateAndExportBindsPass) {
  if (!NEVERD_TEST_WEB_JAVASCRIPT)
    GTEST_SKIP() << "JavaScript parser omitted";
  const auto ID = source();
  const auto Strings = analyze(ID, "string_recovery");
  const auto Constants = analyze(ID, "constant_folding");
  ASSERT_EQ(field(Strings, "analysis_status"), "verified_local_rewrites");
  EXPECT_EQ(field(Strings, "profile"), "javascript-primitive-rewrite-v1");
  EXPECT_NE(field(Strings, "rewrite_id"), field(Constants, "rewrite_id"));
  EXPECT_NE(field(Strings, "export_receipt"),
            field(Constants, "export_receipt"));
  const auto Page = records(field(Strings, "rewrite_id"));
  ASSERT_EQ(Page.getAsObject()->getArray("items")->size(), 2u);
  for (const auto &Item : *Page.getAsObject()->getArray("items")) {
    EXPECT_FALSE(Item.getAsObject()->get("before"));
    EXPECT_FALSE(Item.getAsObject()->get("after"));
    EXPECT_FALSE(Item.getAsObject()->get("before_sha256"));
    EXPECT_FALSE(Item.getAsObject()->get("after_sha256"));
  }
  EXPECT_EQ(code(records(field(Strings, "rewrite_id"), 0, 0)), "invalid_page");
  EXPECT_EQ(
      code(exportTo(Constants, field(Strings, "export_receipt"), Root / "bad")),
      "source_rewrite_export_receipt_mismatch");
  EXPECT_FALSE(std::filesystem::exists(Root / "bad"));
  const auto Out = Root / "export";
  EXPECT_EQ(field(exportTo(Strings, field(Strings, "export_receipt"), Out),
                  "export_status"),
            "verified_local_files");
  EXPECT_EQ(read(Out / "original.js"), Original);
  EXPECT_EQ(read(Out / "candidate.js"),
            "/*CANARY_LICENSE*/const a=(\"CANARY_RECOVERED\");const b=1+2;");
  const auto Receipt = parse(read(Out / "receipt.json"));
  EXPECT_EQ(field(Receipt, "inverse_verification"), "exact_original_sha256");
  EXPECT_EQ(read(Input / "a.js"), Original);
  EXPECT_EQ(code(exportTo(Strings, field(Strings, "export_receipt"), Out)),
            "export_destination_exists");
  EXPECT_EQ(read(Out / "original.js"), Original);
}

TEST_F(WebRewriteSDK, PrimitiveV1ReceiptsRemainExactlyCompatible) {
  if (!NEVERD_TEST_WEB_JAVASCRIPT)
    GTEST_SKIP() << "JavaScript parser omitted";
  const auto Source = source();
  for (const auto &[Pass, File] :
       {std::pair{"string_recovery", "string-recovery-v1-receipt.json"},
        {"constant_folding", "constant-folding-v1-receipt.json"}}) {
    SCOPED_TRACE(Pass);
    const auto GoldenBytes =
        read(std::filesystem::path(NEVERD_WEB_FIXTURE_DIR) / "rewrite" / File);
    ASSERT_FALSE(GoldenBytes.empty());
    const auto Golden = parse(GoldenBytes);
    const auto A = analyze(Source, Pass);
    EXPECT_EQ(field(A, "rewrite_id"), field(Golden, "rewrite_id"));
    EXPECT_EQ(field(A, "export_receipt"), field(Golden, "export_receipt"));
    const auto Page = records(field(A, "rewrite_id"));
    const auto &Rows = *Page.getAsObject()->getArray("items");
    const auto &OldRows = *Golden.getAsObject()->getArray("records");
    ASSERT_EQ(Rows.size(), OldRows.size());
    for (size_t I = 0; I < Rows.size(); ++I)
      EXPECT_EQ(field(Rows[I], "record_id"), field(OldRows[I], "record_id"));
    const auto Out = Root / Pass;
    const auto Exported = exportTo(A, field(A, "export_receipt"), Out);
    EXPECT_EQ(
        field(Exported, "receipt_sha256"),
        std::string_view(Pass) == "string_recovery"
            ? "ec2173260577fc3f4b8d8fb4602500c693fa91fd80e380dbd98878f39e2aa6f2"
            : "8e29fa15d36cece4f8cca5a7587cce4745f4689f52c4adcfe5663753e65e29f"
              "a");
    EXPECT_EQ(read(Out / "receipt.json"), GoldenBytes);
  }
}

TEST_F(WebRewriteSDK, BoundedCacheAndRevisionChangesRevokeOldReceipts) {
  if (!NEVERD_TEST_WEB_JAVASCRIPT)
    GTEST_SKIP() << "JavaScript parser omitted";
  const auto Source = source();
  EXPECT_EQ(code(analyze(Source, "anything")), "unknown_source_rewrite_pass");
  const auto First = analyze(Source, "string_recovery");
  const auto Second = analyze(Source, "constant_folding");
  EXPECT_EQ(field(analyze(Source, "string_recovery"), "rewrite_id"),
            field(First, "rewrite_id"));
  EXPECT_EQ(code(analyze(source(2), "constant_folding")),
            "source_rewrite_cache_budget_exceeded");
  const auto OldRevision = Revision, OldID = field(First, "rewrite_id");
  open();
  EXPECT_NE(Revision, OldRevision);
  EXPECT_EQ(code(take(neverd_web_source_rewrite_records_json(
                S, OldRevision.data(), OldRevision.size(), OldID.data(),
                OldID.size(), 0, 1))),
            "stale_revision");
  EXPECT_EQ(code(records(OldID)), "unknown_source_rewrite");
  const auto Renewed = analyze(source(), "string_recovery");
  EXPECT_EQ(field(Renewed, "rewrite_id"), OldID);
  EXPECT_NE(field(Renewed, "export_receipt"), field(First, "export_receipt"));
  EXPECT_EQ(
      code(exportTo(Renewed, field(First, "export_receipt"), Root / "stale")),
      "source_rewrite_export_receipt_mismatch");
}

TEST_F(WebRewriteSDK, ParseGoalAndImmutableCapturedBytesRemainBoundToReceipt) {
  if (!NEVERD_TEST_WEB_JAVASCRIPT)
    GTEST_SKIP() << "JavaScript parser omitted";
  const auto Script = analyze(source(), "string_recovery");
  const auto &A = Artifacts.at(1);
  const auto CommonJS = take(neverd_web_source_analyze_json(
      S, Revision.data(), Revision.size(), A.data(), A.size(), "commonjs", 8));
  const auto Other = analyze(field(CommonJS, "source_id"), "string_recovery");
  EXPECT_EQ(field(Script, "source_sha256"), field(Other, "source_sha256"));
  EXPECT_EQ(field(Script, "candidate_sha256"),
            field(Other, "candidate_sha256"));
  EXPECT_NE(field(Script, "export_receipt"), field(Other, "export_receipt"));
  EXPECT_EQ(code(exportTo(Other, field(Script, "export_receipt"),
                          Root / "wrong-goal")),
            "source_rewrite_export_receipt_mismatch");
  std::ofstream(Input / "a.js", std::ios::trunc) << "CANARY_CHANGED";
  EXPECT_EQ(field(exportTo(Script, field(Script, "export_receipt"),
                           Root / "captured"),
                  "export_status"),
            "verified_local_files");
  EXPECT_EQ(read(Root / "captured" / "original.js"), Original);
  EXPECT_EQ(read(Input / "a.js"), "CANARY_CHANGED");
}

TEST_F(WebRewriteSDK, CLIUsesSharedReceiptAndWorksWithoutExternalRuntime) {
#ifndef NEVERD_WEB_TEST_CLI
  GTEST_SKIP() << "CLI unavailable";
#else
  if (!NEVERD_TEST_WEB_JAVASCRIPT)
    GTEST_SKIP() << "JavaScript parser omitted";
  const auto Out = (Root / "stdout").string(), Err = (Root / "stderr").string();
  auto Run = [&](std::vector<std::string> Args, int Expected) {
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
              Expected)
        << read(Err);
    std::ifstream Lines(Out);
    std::vector<Value> Results;
    for (std::string Line; std::getline(Lines, Line);)
      if (!Line.empty())
        Results.push_back(parse(Line));
    return Results;
  };
  const auto Preview =
      Run({"rewrite", Input.string(), "script", "string_recovery", "1"}, 0);
  ASSERT_FALSE(Preview.empty());
  const auto Receipt = field(Preview.front(), "export_receipt");
  ASSERT_FALSE(Receipt.empty());
  EXPECT_EQ(field(Preview.front(), "rewrite_id"),
            field(analyze(source(), "string_recovery"), "rewrite_id"));
  Run({"rewrite-export", Input.string(), "script", "constant_folding", "1",
       Receipt, (Root / "wrong-pass").string()},
      1);
  EXPECT_FALSE(std::filesystem::exists(Root / "wrong-pass"));
  Run({"rewrite-export", Input.string(), "script", "string_recovery", "1",
       Receipt, (Root / "cli-export").string()},
      0);
  EXPECT_EQ(read(Root / "cli-export" / "original.js"), Original);
  EXPECT_NE(read(Root / "cli-export" / "candidate.js").find("CANARY_RECOVERED"),
            std::string::npos);
  EXPECT_EQ(read(Input / "a.js"), Original);
#endif
}

} // namespace

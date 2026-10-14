//===- PackageAttestationSDKTests.cpp - Bound offline metadata APIs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original selections, atomic import ceilings, paging, reset and native CLI.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/Base64.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using Value = llvm::json::Value;
using Object = llvm::json::Object;
const char *Statement = R"({"_type":"https://in-toto.io/Statement/v1",
  "subject":[{"name":"CANARY_NAME","digest":{"sha256":
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}}],
  "predicateType":"CANARY_UNSUPPORTED_PREDICATE"})";
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

class WebPackageAttestationSDK : public ::testing::Test {
protected:
  std::filesystem::path Root;
  neverd_web_session_t S = nullptr;
  std::string Revision, Input, Descriptor, Original;
  void write(std::string_view Name, std::string_view Text) {
    std::ofstream Out(Root / std::string(Name), std::ios::binary);
    Out << Text;
  }
  void SetUp() override {
    llvm::SmallString<128> Dir;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-attestation-sdk", Dir));
    Root = Dir.str().str();
    write("a.statement", Statement);
    write("b.descriptor", R"({"schema_version":1,"source":"CANARY_SOURCE",
      "retrieved_at":"2026-10-11T12:00:00Z","as_of":"2026-10-11T11:00:00Z"})");
    write("c.original", "abc");
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
    const auto *Items = Page.getAsObject()->getArray("items");
    return Items && Items->size() == 1 ? field((*Items)[0], "artifact_id") : "";
  }
  void open() {
    const auto Path = Root.string();
    const auto Preview = take(neverd_web_import_preview_json(
        S, Path.data(), Path.size(), nullptr, 0));
    const auto Token = field(Preview, "preview_token");
    const auto Commit =
        take(neverd_web_import_commit_json(S, Token.data(), Token.size()));
    ASSERT_EQ(field(Commit, "status"), "ok");
    Revision = field(Commit, "revision");
    Input = artifact(1);
    Descriptor = artifact(2);
    Original = artifact(3);
  }
  Value import(const std::string &Artifact = {},
               const char *Kind = "intoto-statement") {
    const auto &A = Artifact.empty() ? Input : Artifact;
    return take(neverd_web_package_attestation_import_json(
        S, Revision.data(), Revision.size(), A.data(), A.size(),
        Descriptor.data(), Descriptor.size(), Kind, std::strlen(Kind)));
  }
  Value records(const std::string &ID, const char *Kind = "digests",
                uint64_t Offset = 0, uint64_t Limit = 512) {
    return take(neverd_web_package_attestation_records_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size(), Kind,
        std::strlen(Kind), Offset, Limit));
  }
  std::string digest(const std::string &ID) {
    const auto Page = records(ID);
    return field(Page.getAsObject()->getArray("items")->front(), "digest_id");
  }
  Value match(const std::string &ID, const std::string &Digest,
              const std::string &Artifact = {}) {
    const auto &A = Artifact.empty() ? Original : Artifact;
    return take(neverd_web_package_attestation_digest_match_json(
        S, Revision.data(), Revision.size(), A.data(), A.size(), ID.data(),
        ID.size(), Digest.data(), Digest.size()));
  }
  Value result(const std::string &ID) {
    return take(neverd_web_package_attestation_digest_result_json(
        S, Revision.data(), Revision.size(), ID.data(), ID.size()));
  }
  Value metadata() { return take(neverd_web_metadata_json(S)); }
};

TEST_F(WebPackageAttestationSDK, SelectedOriginalAnchorsAndCachedResultAgree) {
  const auto A = import();
  ASSERT_EQ(field(A, "status"), "ok");
  const auto ID = field(A, "package_attestation_id");
  EXPECT_EQ(field(A, "artifact_id"), Input);
  EXPECT_EQ(field(A, "signature_verification"), "not_performed");
  const auto D = digest(ID);
  const auto M = match(ID, D);
  ASSERT_EQ(field(M, "status"), "ok");
  EXPECT_EQ(field(M, "integrity_status"), "match");
  EXPECT_EQ(field(M, "artifact_id"), Original);
  EXPECT_EQ(field(M, "declaration_artifact_id"), Input);
  EXPECT_EQ(field(M, "declaration_selection_id"), D);
  EXPECT_EQ(field(M, "digest_id"), D);
  EXPECT_EQ(M.getAsObject()->getBoolean("publisher_authenticated"), false);
  EXPECT_EQ(M.getAsObject()->getBoolean("package_inventory_bound"), false);
  EXPECT_EQ(encode(result(field(M, "package_attestation_digest_match_id"))),
            encode(M));
  EXPECT_EQ(encode(match(ID, D)), encode(M));
  EXPECT_EQ(encode(import()), encode(A));
  EXPECT_EQ(metadata().getAsObject()->getInteger("source_inventory_count"), 0);
  EXPECT_EQ(metadata().getAsObject()->getInteger(
                "package_attestation_digest_match_count"),
            1);
  const auto Subjects = records(ID, "subjects");
  EXPECT_EQ(
      field(Subjects.getAsObject()->getArray("items")->front(), "subject_id"),
      field(M, "subject_id"));
}

TEST_F(WebPackageAttestationSDK,
       RefusalsDoNotRetainMetadataAndResetRevokesEveryResult) {
  const auto Before = encode(metadata());
  EXPECT_EQ(code(import({}, "CANARY_KIND")),
            "unsupported_package_attestation_kind");
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(code(import(Original)), "invalid_json");
  EXPECT_EQ(encode(metadata()), Before);
  const auto ID = field(import(), "package_attestation_id");
  const auto D = digest(ID);
  const auto Good = encode(metadata());
  EXPECT_EQ(code(match(ID, std::string(64, '0'))),
            "unknown_package_attestation_digest");
  EXPECT_EQ(code(match(ID, D, std::string(64, '0'))),
            "integrity_original_not_captured");
  EXPECT_EQ(encode(metadata()), Good);
  EXPECT_EQ(code(records(ID, "CANARY_KIND")), "invalid_record_kind");
  EXPECT_EQ(code(records(ID, "digests", 0, 513)), "invalid_page");
  EXPECT_EQ(code(records(ID, "digests", 2)), "invalid_page");
  const auto MatchID =
      field(match(ID, D), "package_attestation_digest_match_id");
  const auto Old = Revision;
  open();
  EXPECT_EQ(code(records(ID)), "unknown_package_attestation");
  EXPECT_EQ(code(result(MatchID)), "unknown_package_attestation_digest_match");
  const auto New = Revision;
  Revision = Old;
  EXPECT_EQ(code(records(ID)), "stale_revision");
  Revision = New;
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_retained_bytes"),
      0);
}

TEST_F(WebPackageAttestationSDK,
       UncomposedImportsConsumeFourSlotsAndShareDescriptorBudget) {
  for (unsigned I = 0; I < 4; ++I)
    write("z" + std::to_string(I), Statement);
  open();
  const auto A = import();
  const auto ID = field(A, "package_attestation_id");
  for (unsigned I = 0; I < 3; ++I)
    ASSERT_EQ(field(import(artifact(4 + I)), "status"), "ok");
  const auto Before = encode(metadata());
  EXPECT_EQ(code(import(artifact(7))),
            "package_attestation_import_cache_budget_exceeded");
  EXPECT_EQ(field(import(), "package_attestation_id"), ID);
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_descriptor_count"),
      1);
  EXPECT_EQ(metadata().getAsObject()->getInteger("package_attestation_count"),
            4);
}

TEST_F(WebPackageAttestationSDK,
       AdvisoryAndAttestationRetainOneDescriptorAndOneAggregateCounter) {
  write("d.advisory", R"({"id":"CANARY","modified":"2026-10-10T00:00:00Z"})");
  open();
  const auto A = import();
  const auto Before =
      metadata().getAsObject()->getInteger("package_evidence_retained_bytes");
  const auto InputID = artifact(4);
  const auto Advisory = take(neverd_web_package_advisory_import_json(
      S, Revision.data(), Revision.size(), InputID.data(), InputID.size(),
      Descriptor.data(), Descriptor.size()));
  ASSERT_EQ(field(Advisory, "status"), "ok");
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_descriptor_count"),
      1);
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_retained_bytes"),
      *Before + *Advisory.getAsObject()->getInteger("retained_metadata_bytes"));
  EXPECT_EQ(field(A, "status"), "ok");
}

TEST_F(WebPackageAttestationSDK,
       SelectedDigestCacheLimitDoesNotDiscardEarlierResults) {
  for (unsigned I = 0; I < 4; ++I)
    write("z" + std::to_string(I), "abc" + std::to_string(I));
  open();
  const auto ID = field(import(), "package_attestation_id");
  const auto D = digest(ID);
  const auto First = match(ID, D);
  for (unsigned I = 0; I < 3; ++I)
    EXPECT_EQ(field(match(ID, D, artifact(4 + I)), "integrity_status"),
              "mismatch");
  const auto Before = encode(metadata());
  EXPECT_EQ(code(match(ID, D, artifact(7))),
            "package_attestation_digest_cache_budget_exceeded");
  EXPECT_EQ(encode(metadata()), Before);
  EXPECT_EQ(encode(match(ID, D)), encode(First));
  EXPECT_EQ(encode(result(field(First, "package_attestation_digest_match_id"))),
            encode(First));
}

TEST_F(WebPackageAttestationSDK,
       PagedSubjectsAndDigestsPreserveEveryOriginalOrdinal) {
  llvm::json::Array Subjects;
  for (unsigned I = 0; I < 64; ++I)
    Subjects.emplace_back(
        Object{{"digest", Object{{"sha256", std::string(64, '0')},
                                 {"sha512", std::string(128, '0')},
                                 {"CANARY_ALGORITHM1", "x"},
                                 {"CANARY_ALGORITHM2", "y"}}}});
  write("a.statement",
        encode(Object{{"_type", "https://in-toto.io/Statement/v1"},
                      {"subject", std::move(Subjects)},
                      {"predicateType", "CANARY"}}));
  open();
  const auto ID = field(import(), "package_attestation_id");
  std::vector<std::string> IDs;
  for (uint64_t Offset = 0; Offset < 256; Offset += 17) {
    const auto Page = records(ID, "digests", Offset, 17);
    ASSERT_EQ(field(Page, "status"), "ok");
    for (const auto &Item : *Page.getAsObject()->getArray("items")) {
      const auto D = field(Item, "digest_id");
      EXPECT_EQ(std::find(IDs.begin(), IDs.end(), D), IDs.end());
      IDs.push_back(D);
    }
    EXPECT_EQ(Page.getAsObject()->getBoolean("page_complete"), Offset == 255);
  }
  EXPECT_EQ(IDs.size(), 256u);
  const auto Page = records(ID, "subjects");
  const auto &Items = *Page.getAsObject()->getArray("items");
  ASSERT_EQ(Items.size(), 64u);
  for (size_t I = 0; I < Items.size(); ++I) {
    EXPECT_EQ(Items[I].getAsObject()->getInteger("array_index"), I);
    EXPECT_EQ(Items[I].getAsObject()->getArray("digest_ids")->size(), 4u);
  }
}

TEST_F(WebPackageAttestationSDK,
       SharedByteCeilingCountsUncomposedAttestations) {
  llvm::json::Array Signatures;
  for (unsigned I = 0; I < 40; ++I)
    Signatures.emplace_back(
        Object{{"keyid", std::string(2000, 'x')},
               {"sig", llvm::encodeBase64(std::string(16384, 's'))}});
  const auto LargeAttestation =
      encode(Object{{"name", "CANARY"},
                    {"version", "1.0.0"},
                    {"dist", Object{{"signatures", std::move(Signatures)}}}});
  ASSERT_LT(LargeAttestation.size(), 1024u * 1024u);
  write("a.statement", LargeAttestation);
  for (unsigned I = 0; I < 3; ++I)
    write("d" + std::to_string(I), LargeAttestation);
  llvm::json::Array Versions;
  for (unsigned I = 0; I < 8192; ++I)
    Versions.emplace_back("1.0.0+" + std::string(90, 'x'));
  const auto LargeAdvisory = encode(
      Object{{"id", "CANARY"},
             {"modified", "2026-10-10T00:00:00Z"},
             {"affected",
              llvm::json::Array{Object{
                  {"package", Object{{"ecosystem", "npm"}, {"name", "CANARY"}}},
                  {"versions", std::move(Versions)}}}}});
  for (unsigned I = 0; I < 16; ++I)
    write("z" + std::to_string(100 + I), LargeAdvisory);
  open();
  const auto First = import({}, "registry-dist");
  ASSERT_EQ(field(First, "status"), "ok");
  for (unsigned I = 0; I < 3; ++I)
    ASSERT_EQ(field(import(artifact(4 + I), "registry-dist"), "status"), "ok");
  const auto Initial =
      *metadata().getAsObject()->getInteger("package_evidence_retained_bytes");
  int64_t PerAdvisory = 0;
  unsigned Accepted = 0;
  for (; Accepted < 16; ++Accepted) {
    const auto Before = encode(metadata());
    const auto InputID = artifact(7 + Accepted);
    const auto A = take(neverd_web_package_advisory_import_json(
        S, Revision.data(), Revision.size(), InputID.data(), InputID.size(),
        Descriptor.data(), Descriptor.size()));
    if (field(A, "status") == "error") {
      EXPECT_EQ(code(A), "package_evidence_retained_budget_exceeded");
      EXPECT_EQ(encode(metadata()), Before);
      break;
    }
    ASSERT_EQ(field(A, "status"), "ok");
    PerAdvisory = *A.getAsObject()->getInteger("retained_metadata_bytes");
  }
  ASSERT_GT(Accepted, 0u);
  ASSERT_LT(Accepted, 16u);
  const auto Total = Initial + Accepted * PerAdvisory;
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_retained_bytes"),
      Total);
  EXPECT_LE(Total, 8 * 1024 * 1024);
  EXPECT_GT(Total + PerAdvisory, 8 * 1024 * 1024);
  EXPECT_LT((Accepted + 1) * PerAdvisory, 8 * 1024 * 1024);
  EXPECT_EQ(
      metadata().getAsObject()->getInteger("package_evidence_descriptor_count"),
      1);
  EXPECT_EQ(encode(import({}, "registry-dist")), encode(First));
}

#ifdef NEVERD_WEB_TEST_CLI
TEST_F(WebPackageAttestationSDK, StandaloneCliUsesNativeApisWithUnusablePath) {
  const auto ID = field(import(), "package_attestation_id");
  const auto Expected = match(ID, digest(ID));
  const auto Out = Root.string() + ".stdout", Err = Root.string() + ".stderr";
  const auto Path = Root.string();
  // Creating and unlinking a spool in the capture changes its directory
  // metadata. Keep temporary storage outside the immutable input root.
  const std::string TmpEnv = "TMPDIR=" + Root.parent_path().string();
  const std::vector<llvm::StringRef> Env{"PATH=/neverd-unavailable", TmpEnv};
  const std::vector<std::optional<llvm::StringRef>> Redirects{"", Out, Err};
  for (bool Selected : {false, true}) {
    std::vector<llvm::StringRef> Args{
        NEVERD_WEB_TEST_CLI, "web", "package-attestation", Path, "1", "2",
        "intoto-statement"};
    if (Selected) {
      Args.push_back("3");
      Args.push_back("0");
    }
    std::ofstream(Out, std::ios::trunc).close();
    std::ofstream(Err, std::ios::trunc).close();
    const auto Exit = llvm::sys::ExecuteAndWait(NEVERD_WEB_TEST_CLI, Args, Env,
                                                Redirects, 30);
    const auto Read = [](const std::string &Path) {
      std::ifstream Stream(Path);
      return std::string(std::istreambuf_iterator<char>(Stream), {});
    };
    ASSERT_EQ(Exit, 0) << Read(Out) << Read(Err);
    std::ifstream File(Out);
    std::string Line;
    unsigned Lines = 0;
    while (std::getline(File, Line)) {
      EXPECT_EQ(Line.find("CANARY"), Line.npos);
      auto V = llvm::json::parse(Line);
      ASSERT_TRUE(bool(V));
      EXPECT_EQ(field(*V, "status"), "ok");
      if (V->getAsObject()->get("package_attestation_digest_match_id"))
        EXPECT_EQ(field(*V, "package_attestation_digest_match_id"),
                  field(Expected, "package_attestation_digest_match_id"));
      ++Lines;
    }
    EXPECT_EQ(Lines, Selected ? 5u : 4u);
    EXPECT_EQ(std::filesystem::file_size(Err), 0u);
  }
}
#endif
} // namespace

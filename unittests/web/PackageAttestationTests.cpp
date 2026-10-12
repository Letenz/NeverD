//===- PackageAttestationTests.cpp - Captured metadata boundaries -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent encoding, digest, chronology and untrusted provenance fixtures.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "gtest/gtest.h"
#include "packages/attestations/Encoding.h"
#include "packages/attestations/Publication.h"

#include "neverd/web/Error.h"

#include "llvm/Support/Base64.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstdlib>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::package_attestations;
using Value = llvm::json::Value;
using Object = llvm::json::Object;

const char *SHA256ABC =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const char *SHA512ABC =
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";

class WebPackageAttestations : public ::testing::Test {
protected:
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX blob storage profile required";
#endif
  }
};
Artifact artifact(std::string_view Text) {
  BlobStore Store(2 * 1024 * 1024);
  Store.append(Text);
  Store.seal();
  Artifact A;
  A.Content = Store.whole();
  A.BlobHash = A.Content.digest();
  A.ID = identity("attestation-test-artifact", {A.BlobHash});
  return A;
}
PackageEvidenceDescriptor descriptor() {
  return readPackageEvidenceDescriptor(artifact(R"({"schema_version":1,
    "source":"CANARY_URI","retrieved_at":"2026-10-11T12:00:00Z",
    "as_of":"2026-10-11T11:00:00Z"})"));
}
std::string encode(Value V) { return llvm::formatv("{0}", V).str(); }
Object statement() {
  return Object{
      {"_type", "https://in-toto.io/Statement/v1"},
      {"subject",
       llvm::json::Array{Object{
           {"name", "CANARY_NAME"},
           {"uri", "CANARY_URI"},
           {"digest", Object{{"sha256", SHA256ABC}, {"sha512", SHA512ABC}}}}}},
      {"predicateType", "https://slsa.dev/provenance/v1"},
      {"predicate",
       Object{
           {"buildDefinition",
            Object{
                {"buildType", "CANARY_BUILD_TYPE"},
                {"externalParameters", Object{{"CANARY_EXT", "CANARY_VALUE"}}},
                {"internalParameters", Object{{"CANARY_INT", "CANARY_VALUE"}}},
                {"resolvedDependencies",
                 llvm::json::Array{Object{{"uri", "CANARY_DEPENDENCY"}}}}}},
           {"runDetails",
            Object{{"builder", Object{{"id", "CANARY_BUILDER"}}},
                   {"metadata",
                    Object{{"startedOn", "2026-10-10T09:00:00Z"},
                           {"finishedOn", "2026-10-10T10:00:00Z"}}}}}}}};
}
Object envelope(std::string Payload) {
  return Object{{"payloadType", "application/vnd.in-toto+json"},
                {"payload", llvm::encodeBase64(Payload)},
                {"signatures", llvm::json::Array{Object{{"keyid", "CANARY_KEY"},
                                                        {"sig", "Zm9v"}}}}};
}
PackageAttestation read(Object O, std::string_view Kind = "intoto-statement") {
  return readPackageAttestation(artifact(encode(std::move(O))), descriptor(),
                                Kind);
}
void privatePublication(const PackageAttestation &A) {
  const auto Check = [](Object O) {
    EXPECT_EQ(encode(std::move(O)).find("CANARY"), std::string::npos);
  };
  Check(summary(A, 9));
  for (const auto &S : A.Signatures)
    Check(record(S));
  for (const auto &S : A.Subjects)
    Check(record(S));
  for (const auto &D : A.Digests)
    Check(record(D));
  EXPECT_EQ(summary(A, 9).getString("signature_verification"), "not_performed");
  EXPECT_EQ(summary(A, 9).getBoolean("publisher_authenticated"), false);
  EXPECT_EQ(summary(A, 9).getBoolean("slsa_level_verified"), false);
}

TEST(WebAttestationEncoding, RFC4648VectorsAndExplicitAlphabetSelection) {
  for (const auto &[Encoded, Bytes] :
       std::vector<std::pair<std::string, std::string>>{{"Zg==", "f"},
                                                        {"Zm8=", "fo"},
                                                        {"Zm9v", "foo"},
                                                        {"Zm9vYg==", "foob"},
                                                        {"Zm9vYmE=", "fooba"},
                                                        {"Zm9vYmFy", "foobar"},
                                                        {"Zg", "f"},
                                                        {"Zm8", "fo"}}) {
    const auto D = decodeBase64(Encoded, 6, false);
    EXPECT_EQ(D.Status, "admitted_standard_base64");
    EXPECT_EQ(D.Bytes, Bytes);
  }
  EXPECT_EQ(decodeBase64("", 0, true).Status, "empty_base64");
  EXPECT_EQ(decodeBase64("-___", 3, true).Bytes,
            std::string("\xfb\xff\xff", 3));
  EXPECT_EQ(decodeBase64("+///", 3, true).Status, "admitted_standard_base64");
  EXPECT_EQ(decodeBase64("-___", 3, true).Status, "admitted_url_base64");
  EXPECT_EQ(decodeBase64("-___", 3, false).Status,
            "unsupported_base64_alphabet");
  EXPECT_EQ(decodeBase64("+___", 3, true).Status,
            "unsupported_base64_alphabet");
}

TEST(WebAttestationEncoding, MalformedAndOversizedBytesAreNeverCoerced) {
  for (const auto *Text : {"Z", "=", "Zg=", "Zg===", "Zg==junk", "Zg==\n",
                           " Zg==", "Zh==", "Zm9=", "Zg!", "Zm9v="}) {
    const auto D = decodeBase64(Text, 100, true);
    EXPECT_EQ(D.Status, "invalid_base64") << Text;
    EXPECT_TRUE(D.Bytes.empty());
  }
  EXPECT_EQ(decodeBase64("Zm9v", 2, true).Status,
            "decoded_byte_budget_exceeded");
}

TEST_F(WebPackageAttestations,
       CapturedRegistryDeclaresFieldsWithoutKeysOrTrust) {
  auto A = read(
      Object{{"name", "CANARY_PACKAGE"},
             {"version", "1.2.3"},
             {"_id", "CANARY_PACKAGE@1.2.3"},
             {"dist",
              Object{{"integrity",
                      "sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0="},
                     {"attestations", Object{{"url", "CANARY_REMOTE_URL"}}},
                     {"signatures",
                      llvm::json::Array{
                          Object{{"keyid", "CANARY_KEY"}, {"sig", "Zm9v"}},
                          Object{{"keyid", "CANARY_KEY"}, {"sig", "Zm8="}}}}}}},
      "registry-dist");
  EXPECT_EQ(A.PackageIdentityStatus, "name_and_version_declared");
  EXPECT_EQ(A.IntegrityStatus, "declared_unverified");
  ASSERT_EQ(A.Signatures.size(), 2u);
  EXPECT_NE(A.Signatures[0].ID, A.Signatures[1].ID);
  EXPECT_EQ(A.Signatures[0].KeyID, A.Signatures[1].KeyID);
  EXPECT_EQ(A.Signatures[0].Bytes, "foo");
  EXPECT_TRUE(A.Subjects.empty());
  privatePublication(A);
}

TEST_F(WebPackageAttestations,
       RegistryAbsenceConflictAndInvalidEncodingRemainVisible) {
  EXPECT_EQ(read(Object{}, "registry-dist").PackageIdentityStatus,
            "package_declaration_incomplete");
  auto A = read(
      Object{{"name", "CANARY"},
             {"version", "1.0.0"},
             {"_id", "CANARY@2.0.0"},
             {"dist",
              Object{{"signatures", llvm::json::Array{Object{{"sig", "Zh=="}},
                                                      Object{{"sig", ""}}}}}}},
      "registry-dist");
  EXPECT_EQ(A.PackageIdentityStatus, "conflicting_registry_declarations");
  EXPECT_EQ(A.IntegrityStatus, "not_supplied");
  ASSERT_EQ(A.Signatures.size(), 2u);
  EXPECT_EQ(A.Signatures[0].EncodingStatus, "invalid_base64");
  EXPECT_EQ(A.Signatures[1].EncodingStatus, "empty_base64");
  privatePublication(A);
}

TEST_F(WebPackageAttestations,
       SlsaMetadataIsNeitherLevelNorDependencyVerification) {
  const auto A = read(statement());
  EXPECT_EQ(A.StatementStatus, "admitted_statement_v1_subset");
  EXPECT_EQ(A.PredicateStatus, "supplied_slsa_v1_metadata_subset");
  EXPECT_EQ(A.BuildTimeStatus, "declared_build_times_consistent");
  EXPECT_EQ(A.ExternalParameterCount, 1u);
  EXPECT_EQ(A.InternalParameterCount, 1u);
  EXPECT_EQ(A.ResolvedDependencyCount, 1u);
  EXPECT_EQ(A.Digests.size(), 2u);
  EXPECT_EQ(summary(A, 1).getBoolean("resolved_dependency_digests_verified"),
            false);
  privatePublication(A);
}

TEST_F(WebPackageAttestations,
       ExactDsseBytesRetainedAndUnverifiedSignaturesIndependent) {
  const auto Payload = " \n" + encode(statement()) + "\n";
  const auto A = read(envelope(Payload), "dsse-intoto");
  EXPECT_EQ(A.Payload, Payload);
  EXPECT_EQ(A.PayloadHash, sha256(Payload));
  EXPECT_EQ(A.PayloadStatus, "admitted_standard_base64");
  EXPECT_EQ(A.StatementStatus, "admitted_statement_v1_subset");
  ASSERT_EQ(A.Signatures.size(), 1u);
  EXPECT_EQ(A.Signatures[0].Bytes, "foo");
  privatePublication(A);
  auto Invalid = envelope(Payload);
  Invalid["payload"] = "Zh==";
  const auto Bad = read(std::move(Invalid), "dsse-intoto");
  EXPECT_EQ(Bad.PayloadStatus, "invalid_base64");
  EXPECT_TRUE(Bad.Subjects.empty());
  auto Unknown = envelope(Payload);
  Unknown["payloadType"] = "CANARY_UNSUPPORTED_TYPE";
  const auto Other = read(std::move(Unknown), "dsse-intoto");
  EXPECT_EQ(Other.PayloadStatus, "unsupported_payload_type");
  EXPECT_TRUE(Other.Subjects.empty());
  privatePublication(Other);
}

TEST_F(WebPackageAttestations,
       PartialAndUnknownStatementsDoNotPretendToValidate) {
  EXPECT_EQ(read(Object{}).StatementStatus, "statement_type_not_supplied");
  EXPECT_THROW(read(Object{{"_type", 1}}), Error);
  for (const auto &Missing : {"subject", "predicateType"}) {
    auto O = statement();
    O.erase(Missing);
    EXPECT_EQ(read(std::move(O)).StatementStatus, "incomplete_statement_v1");
  }
  auto O = statement();
  O["subject"] = llvm::json::Array{Object{{"name", "CANARY"}}};
  const auto A = read(std::move(O));
  EXPECT_EQ(A.StatementStatus, "incomplete_statement_v1");
  EXPECT_EQ(record(A.Subjects[0]).getString("digest_status"), "not_supplied");
  O = statement();
  O["_type"] = "CANARY_UNSUPPORTED_STATEMENT";
  EXPECT_EQ(read(std::move(O)).StatementStatus, "unsupported_statement_type");
  O = statement();
  O["predicateType"] = "CANARY_OTHER_PREDICATE";
  EXPECT_EQ(read(std::move(O)).PredicateStatus, "unsupported_predicate_type");
}

TEST_F(WebPackageAttestations,
       UnsupportedDigestsAndInvalidHexAreSeparateStates) {
  auto O = statement();
  O["subject"] = llvm::json::Array{
      Object{{"digest", Object{{"sha256", "CANARY_NOT_HEX"},
                               {"CANARY_ALGORITHM", "CANARY_VALUE"}}}}};
  const auto A = read(std::move(O));
  ASSERT_EQ(A.Digests.size(), 2u);
  EXPECT_EQ(A.Digests[0].Validation, "unsupported_digest_algorithm");
  EXPECT_EQ(A.Digests[1].Validation, "invalid_hex_digest");
  for (const auto &D : A.Digests)
    EXPECT_THROW(matchPackageAttestationDigest(artifact("abc"), A, D.ID),
                 Error);
  privatePublication(A);
}

TEST_F(WebPackageAttestations,
       DigestEqualityBindsOnlyTheExplicitOriginalAndSubject) {
  const auto A = read(statement());
  const auto Original = artifact("abc");
  for (const auto &D : A.Digests) {
    const auto M = matchPackageAttestationDigest(Original, A, D.ID);
    EXPECT_EQ(M.Integrity.Status, "match");
    EXPECT_EQ(M.Integrity.ArtifactID, Original.ID);
    EXPECT_EQ(M.Integrity.DeclarationArtifactID, A.ArtifactID);
    EXPECT_EQ(M.Integrity.SelectionID, D.ID);
    EXPECT_EQ(M.SubjectID, D.SubjectID);
    EXPECT_EQ(summary(M, 1).getBoolean("publisher_authenticated"), false);
    EXPECT_EQ(summary(M, 1).getBoolean("package_inventory_bound"), false);
    EXPECT_EQ(summary(M, 1).getBoolean("other_subjects_or_digests_checked"),
              false);
    EXPECT_EQ(matchPackageAttestationDigest(artifact("abcd"), A, D.ID)
                  .Integrity.Status,
              "mismatch");
  }
  auto Upper = statement();
  Upper["subject"] = llvm::json::Array{
      Object{{"digest", Object{{"sha256", "BA7816BF8F01CFEA414140DE5DAE2223B003"
                                          "61A396177A9CB410FF61F20015AD"}}}}};
  const auto U = read(std::move(Upper));
  EXPECT_EQ(matchPackageAttestationDigest(Original, U, U.Digests[0].ID)
                .Integrity.Status,
            "match");
}

TEST_F(WebPackageAttestations,
       ChronologyAndOverlappingParametersRemainDeclarations) {
  for (const auto &[Time, Status] :
       std::vector<std::pair<std::string, std::string>>{
           {"CANARY_TIME", "unsupported_or_invalid_build_time"},
           {"", "incomplete_build_times"},
           {"2026-10-09T00:00:00Z", "conflicting_build_times"},
           {"2026-10-12T00:00:00Z", "build_newer_than_supplied_as_of"}}) {
    auto O = statement();
    (*O.getObject("predicate")
          ->getObject("runDetails")
          ->getObject("metadata"))["finishedOn"] = Time;
    const auto A = read(std::move(O));
    EXPECT_EQ(A.BuildTimeStatus, Status);
    privatePublication(A);
  }
  auto O = statement();
  (*O.getObject("predicate")
        ->getObject("buildDefinition"))["internalParameters"] =
      Object{{"CANARY_EXT", 1}};
  EXPECT_EQ(read(std::move(O)).PredicateStatus,
            "overlapping_parameter_declarations");
  const auto D =
      readPackageEvidenceDescriptor(artifact(R"({"schema_version":1})"));
  EXPECT_EQ(readPackageAttestation(artifact(encode(statement())), D,
                                   "intoto-statement")
                .BuildTimeStatus,
            "source_time_coverage_unavailable");
}

TEST_F(WebPackageAttestations,
       HashSelectionAndRequiredFieldFailuresAreExplicit) {
  auto Input = artifact(encode(statement()));
  Input.BlobHash = std::string(64, '0');
  EXPECT_THROW(readPackageAttestation(Input, descriptor(), "intoto-statement"),
               Error);
  EXPECT_THROW(read(statement(), "CANARY_PROFILE"), Error);
  for (const auto *Missing : {"payloadType", "payload", "signatures"}) {
    auto O = envelope(encode(statement()));
    O.erase(Missing);
    EXPECT_THROW(read(std::move(O), "dsse-intoto"), Error);
  }
  auto O = envelope("{malformed");
  EXPECT_THROW(read(std::move(O), "dsse-intoto"), Error);
  auto A = read(statement());
  const auto Original = artifact("abc");
  EXPECT_THROW(matchPackageAttestationDigest(Original, A, std::string(64, '0')),
               Error);
  A.Subjects[0].Index = 1;
  EXPECT_THROW(matchPackageAttestationDigest(Original, A, A.Digests[0].ID),
               Error);
  A = read(statement());
  A.Digests[0].SubjectID = std::string(64, '0');
  EXPECT_THROW(matchPackageAttestationDigest(Original, A, A.Digests[0].ID),
               Error);
}

TEST_F(WebPackageAttestations,
       ImportCountsAndDecodedBudgetsApplyBeforeRetention) {
  auto O = statement();
  llvm::json::Array Subjects;
  for (size_t I = 0; I <= MaxPackageAttestationSubjects; ++I)
    Subjects.emplace_back(Object{});
  O["subject"] = std::move(Subjects);
  EXPECT_THROW(read(std::move(O)), Error);
  auto E = envelope(encode(statement()));
  llvm::json::Array Signatures;
  for (size_t I = 0; I <= MaxPackageAttestationSignatures; ++I)
    Signatures.emplace_back(Object{{"sig", "Zm9v"}});
  E["signatures"] = std::move(Signatures);
  EXPECT_THROW(read(std::move(E), "dsse-intoto"), Error);
  E = envelope(std::string(MaxPackageAttestationPayloadBytes + 1, ' '));
  EXPECT_THROW(read(std::move(E), "dsse-intoto"), Error);
  E = envelope(encode(statement()));
  E["signatures"] = llvm::json::Array{
      Object{{"sig", llvm::encodeBase64(std::string(
                         MaxPackageAttestationSignatureBytes + 1, 'x'))}}};
  EXPECT_THROW(read(std::move(E), "dsse-intoto"), Error);
  O = statement();
  Object Digests;
  for (size_t I = 0; I <= MaxPackageAttestationDigests; ++I)
    Digests["CANARY_" + std::to_string(I)] = "x";
  O["subject"] = llvm::json::Array{Object{{"digest", std::move(Digests)}}};
  EXPECT_THROW(read(std::move(O)), Error);
}

TEST_F(WebPackageAttestations,
       OptionalCapturedClaudeRegistryRetainsTwoUnverifiedSignatures) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21295_REGISTRY");
  if (!Path)
    GTEST_SKIP() << "Optional pinned captured registry metadata not supplied";
  std::ifstream File(Path, std::ios::binary);
  const std::string Bytes((std::istreambuf_iterator<char>(File)), {});
  ASSERT_EQ(sha256(Bytes),
            "5b08bcfe0bd28ab796d40388a360e8f5b5d5db672376106887972e0715109578");
  const auto A =
      readPackageAttestation(artifact(Bytes), descriptor(), "registry-dist");
  EXPECT_EQ(A.Name, "@anthropic-ai/claude-code");
  EXPECT_EQ(A.Version, "2.1.295");
  EXPECT_EQ(A.PackageIdentityStatus, "name_and_version_declared");
  ASSERT_EQ(A.Signatures.size(), 2u);
  for (const auto &S : A.Signatures) {
    EXPECT_EQ(S.EncodingStatus, "admitted_standard_base64");
    EXPECT_EQ(S.Bytes.size(), 72u);
  }
  EXPECT_EQ(A.Signatures[0].KeyID, A.Signatures[1].KeyID);
  EXPECT_NE(A.Signatures[0].Bytes, A.Signatures[1].Bytes);
  privatePublication(A);
}
} // namespace

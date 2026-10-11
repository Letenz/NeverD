//===- PackageAssessmentTests.cpp - Cross-owner package review evidence --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent identity, archive ancestry, coverage and private report cases.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "PackageArchiveFixture.h"
#include "gtest/gtest.h"
#include "packages/assessment/Publication.h"

#include "neverd/web/Error.h"
#include "neverd/web/NodeBehavior.h"
#include "neverd/web/PackageAdvisories.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/PackageAttestations.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <array>

namespace {
using namespace neverd::web;
using namespace neverd::web::package_assessment;

class WebPackageAssessment : public ::testing::Test {
protected:
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX blob storage required";
#endif
  }
};
Artifact artifact(std::string_view Text, std::string_view Path = {}) {
  BlobStore Store(2 * 1024 * 1024);
  Store.append(Text);
  Store.seal();
  Artifact A;
  A.Content = Store.whole();
  A.BlobHash = A.Content.digest();
  A.ID = identity("assessment-test-artifact", {A.BlobHash, Path});
  A.MemberPath = Path;
  return A;
}
PackageEvidenceDescriptor descriptor() {
  return readPackageEvidenceDescriptor(artifact(R"({"schema_version":1,
    "source":"CANARY_SOURCE_URI", "retrieved_at":"2026-10-11T12:00:00Z",
    "as_of":"2026-10-11T11:00:00Z"})"));
}
Snapshot inventory() {
  Snapshot N;
  N.ID = identity("assessment-test-namespace", {"foo"});
  Artifact Root;
  Root.ID = N.ID;
  Root.Directory = true;
  N.Artifacts = {
      Root, artifact(R"({"name":"foo","version":"1.0.0"})", "package.json")};
  return N;
}
PackageAttestation registry(std::string_view Name, const Artifact &Original) {
  const auto SRI =
      "sha256-" + llvm::encodeBase64(llvm::fromHex(Original.BlobHash));
  return readPackageAttestation(
      artifact("{\"name\":\"" + std::string(Name) +
               "\",\"version\":\"1.0.0\",\"dist\":{\"integrity\":\"" + SRI +
               "\",\"signatures\":[{\"keyid\":\"CANARY_KEY\","
               "\"sig\":\"YWJj\"}]}}"),
      descriptor(), "registry-dist");
}
PackageIntegrityResult receipt(const PackageAttestation &T,
                               const Artifact &Original) {
  return verifyPackageIntegrity(Original,
                                {T.ArtifactID, T.BlobHash, T.ArtifactID,
                                 "registry_dist", T.Integrity, false});
}
const PackageFinding &finding(const PackageAssessment &A,
                              PackageFindingKind Kind) {
  const auto F = std::find_if(A.Findings.begin(), A.Findings.end(),
                              [&](const auto &R) { return R.Kind == Kind; });
  if (F == A.Findings.end())
    throw Error("test_finding_absent");
  return *F;
}
template <class Action> void fails(Action Run, const char *Code) {
  try {
    Run();
    FAIL() << "Expected refusal";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), Code);
  }
}

TEST_F(WebPackageAssessment, MissingSelectionsRemainExplicitCoverage) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto A = composePackageAssessment(P, N, {});
  EXPECT_EQ(A.NamespaceID, N.ID);
  EXPECT_EQ(A.PackageAnalysisID, P.ID);
  EXPECT_EQ(std::count_if(A.Findings.begin(), A.Findings.end(),
                          [](const auto &F) {
                            return F.Kind == PackageFindingKind::Coverage &&
                                   F.Status == "not_selected";
                          }),
            8);
  EXPECT_EQ(finding(A, PackageFindingKind::Inventory).Status,
            "identity_declared");
  const auto R = createPackageReview(A);
  const auto Report = markdown(A, R, 0, 512);
  EXPECT_NE(Report.find("no safe-package conclusion"), std::string::npos);
  EXPECT_NE(Report.find("unreviewed"), std::string::npos);
  EXPECT_EQ(Report.find("foo"), std::string::npos);
}

TEST_F(WebPackageAssessment, AValidForeignRegistryReceiptCannotVerifyFoo) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto Original = artifact("abc");
  Snapshot Originals;
  Originals.ID = identity("assessment-originals", {Original.ID});
  Originals.Artifacts = {Original};
  const auto T = registry("bar", Original);
  const auto I = receipt(T, Original);
  ASSERT_EQ(I.Status, "match");
  const std::array<const PackageIntegrityResult *, 1> Receipts{&I};
  const std::array<const PackageAttestation *, 1> Metadata{&T};
  PackageAssessmentSelection S;
  S.Originals = &Originals;
  S.Integrity = Receipts;
  S.Attestations = Metadata;
  fails([&] { (void)composePackageAssessment(P, N, S); },
        "package_assessment_declaration_mismatch");
}

TEST_F(WebPackageAssessment, SameIdentityInALooseDirectoryRemainsUnbound) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto Original = artifact("abc");
  Snapshot Originals;
  Originals.ID = identity("assessment-originals", {Original.ID});
  Originals.Artifacts = {Original};
  const auto T = registry("foo", Original);
  const auto I = receipt(T, Original);
  const std::array<const PackageIntegrityResult *, 1> Receipts{&I};
  const std::array<const PackageAttestation *, 1> Metadata{&T};
  PackageAssessmentSelection S;
  S.Originals = &Originals;
  S.Integrity = Receipts;
  S.Attestations = Metadata;
  const auto A = composePackageAssessment(P, N, S);
  const auto &F = finding(A, PackageFindingKind::Integrity);
  EXPECT_EQ(F.Status, "match");
  EXPECT_EQ(F.Binding, "declared_identity_original_inventory_unbound");
  EXPECT_EQ(F.PackageID, P.Packages.front().ID);
  EXPECT_EQ(F.DeclarationArtifactID, T.ArtifactID);
  // The identity preimage uses the original snapshot's retained storage.
  // Conditional conversions to temporary strings must not back its views.
  std::array<std::string, 2> IDs{I.ID, T.ID};
  std::sort(IDs.begin(), IDs.end());
  EXPECT_EQ(A.ID, identity("package-assessment",
                           {PackageAssessmentProfile, P.ID, N.ID, "",
                            Originals.ID, IDs[0], IDs[1]}));
}

TEST_F(WebPackageAssessment, LegacyDigestStrengthRemainsASeparateSignal) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto Original = artifact("abc");
  const auto Declaration = artifact(R"({"dist":{
    "integrity":"sha1-qZk+NkcGgWq6PiVxeFDCbJzQ2J0="}})");
  const auto I =
      verifyPackageIntegrity(Original, registryPackageIntegrity(Declaration));
  ASSERT_EQ(I.Status, "match");
  Snapshot Originals;
  Originals.ID = identity("assessment-originals", {Original.ID});
  Originals.Artifacts = {Original};
  const std::array<const PackageIntegrityResult *, 1> Receipts{&I};
  PackageAssessmentSelection S;
  S.Originals = &Originals;
  S.Integrity = Receipts;
  const auto A = composePackageAssessment(P, N, S);
  const auto &F = finding(A, PackageFindingKind::Integrity);
  EXPECT_EQ(F.Status, "match");
  EXPECT_EQ(F.DigestAlgorithm, "sha1");
  EXPECT_EQ(record(F, std::nullopt).getBoolean("legacy_sha1"), true);
  EXPECT_NE(markdown(A, createPackageReview(A), 0, 512).find("legacy SHA-1"),
            std::string::npos);
}

TEST_F(WebPackageAssessment, SelectionOrderCannotChangeFindingIdentities) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto O1 = artifact("abc"), O2 = artifact("abd");
  Snapshot Originals;
  Originals.ID = identity("assessment-originals", {O1.ID, O2.ID});
  Originals.Artifacts = {O1, O2};
  const auto T1 = registry("foo", O1), T2 = registry("foo", O2);
  const auto I1 = receipt(T1, O1), I2 = receipt(T2, O2);
  std::array<const PackageIntegrityResult *, 2> Receipts{&I1, &I2};
  std::array<const PackageAttestation *, 2> Metadata{&T1, &T2};
  PackageAssessmentSelection S;
  S.Originals = &Originals;
  S.Integrity = Receipts;
  S.Attestations = Metadata;
  const auto A = composePackageAssessment(P, N, S);
  std::reverse(Receipts.begin(), Receipts.end());
  std::reverse(Metadata.begin(), Metadata.end());
  const auto B = composePackageAssessment(P, N, S);
  EXPECT_EQ(A.ID, B.ID);
  EXPECT_EQ(A.SelectionIDs, B.SelectionIDs);
  ASSERT_EQ(A.Findings.size(), B.Findings.size());
  for (size_t I = 0; I < A.Findings.size(); ++I)
    EXPECT_EQ(A.Findings[I].ID, B.Findings[I].ID);
  Receipts[0] = Receipts[1];
  fails([&] { (void)composePackageAssessment(P, N, S); },
        "package_assessment_duplicate_selection");
}

TEST_F(WebPackageAssessment, UnsupportedAdvisoryCoverageCannotBecomeNegative) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto D = readPackageAdvisory(artifact(R"({"id":"CANARY_ID",
    "modified":"2026-10-10T00:00:00Z", "affected":[{"package":{
    "ecosystem":"npm","name":"foo"},"ranges":[{"type":"ECOSYSTEM",
    "events":[{"introduced":"0"},{"fixed":"2.0.0"}]}]}]})"),
                                     descriptor());
  const std::vector<const PackageAdvisory *> Feeds{&D};
  auto M = matchPackageAdvisories(P, Feeds);
  ASSERT_EQ(M.Records.size(), 1u);
  ASSERT_EQ(M.Records[0].Status, "coverage_unknown");
  PackageAssessmentSelection S;
  S.Advisories = Feeds;
  S.AdvisoryMatches = &M;
  const auto A = composePackageAssessment(P, N, S);
  const auto &F = finding(A, PackageFindingKind::Advisory);
  EXPECT_EQ(F.Status, "coverage_unknown");
  EXPECT_EQ(F.ArtifactID, D.ArtifactID);
  EXPECT_EQ(F.RecordID, M.Records[0].ID);
  EXPECT_NE(
      markdown(A, createPackageReview(A), 0, 512).find("coverage_unknown"),
      std::string::npos);
  M.AdvisoryIDs[0] = identity("foreign-advisory", {"other"});
  fails([&] { (void)composePackageAssessment(P, N, S); },
        "package_assessment_advisory_mismatch");
}

TEST_F(WebPackageAssessment, ArchiveAncestryBindsOnlyTheDeclaredRootManifest) {
  if (!packageArchiveAvailable())
    GTEST_SKIP()
        << "Archive ancestry requires the optional archive path policy";
  const auto Original = artifact(test::finishTar(test::tarMember(
      "package/package.json", R"({"name":"foo","version":"1.0.0"})")));
  const auto Archive = extractPackageArchive(Original, "tar");
  const auto N = packageArchiveNamespace(Archive);
  ASSERT_EQ(N.Artifacts.size(), 2u);
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto T = registry("foo", Original);
  const auto I = receipt(T, Original);
  Snapshot Originals;
  Originals.ID = identity("assessment-originals", {Original.ID});
  Originals.Artifacts = {Original};
  const std::array<const PackageArchive *, 1> Archives{&Archive};
  const std::array<const PackageIntegrityResult *, 1> Receipts{&I};
  const std::array<const PackageAttestation *, 1> Metadata{&T};
  PackageAssessmentSelection S;
  S.Originals = &Originals;
  S.Archives = Archives;
  S.Integrity = Receipts;
  S.Attestations = Metadata;
  const auto A = composePackageAssessment(P, N, S);
  EXPECT_EQ(finding(A, PackageFindingKind::Integrity).Binding,
            "original_contains_declared_root_manifest");
  std::array<std::string, 2> IDs{I.ID, T.ID};
  std::sort(IDs.begin(), IDs.end());
  EXPECT_EQ(A.ID, identity("package-assessment",
                           {PackageAssessmentProfile, P.ID, N.ID, Archive.ID,
                            Originals.ID, IDs[0], IDs[1]}));
  auto ForeignNamespace = N;
  ForeignNamespace.ID = identity("foreign-namespace", {N.ID});
  fails([&] { (void)composePackageAssessment(P, ForeignNamespace, S); },
        "package_assessment_namespace_mismatch");
  const auto R = createPackageReview(A);
  const auto Report = markdown(A, R, 0, 512);
  EXPECT_EQ(Report.find("CANARY"), std::string::npos);
  EXPECT_EQ(Report.find("YWJj"), std::string::npos);
  EXPECT_NE(Report.find("2026-10-11T12:00:00Z"), std::string::npos);
  EXPECT_NE(Report.find("does not verify an installed dependency tree"),
            std::string::npos);
}

TEST_F(WebPackageAssessment,
       OptionalBehaviorAndComparisonMustUseTheExactOwner) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  PackageBehavior B;
  B.ID = identity("assessment-test-behavior", {"other"});
  B.PackageAnalysisID = identity("assessment-test-analysis", {"other"});
  PackageAssessmentSelection S;
  S.Behavior = &B;
  fails([&] { (void)composePackageAssessment(P, N, S); },
        "package_assessment_behavior_mismatch");
  S.Behavior = nullptr;
  PackageDiff D;
  D.ID = identity("assessment-test-diff", {"other"});
  D.BeforeID = D.AfterID = B.PackageAnalysisID;
  S.Comparison = &D;
  fails([&] { (void)composePackageAssessment(P, N, S); },
        "package_assessment_comparison_mismatch");
}

TEST_F(WebPackageAssessment, DispositionsChangeOnlyTheReviewHistoryAndReport) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  const auto A = composePackageAssessment(P, N, {});
  const auto R0 = createPackageReview(A);
  const auto R1 = appendPackageReview(A, R0, R0.HeadID, A.Findings.front().ID,
                                      PackageDisposition::AcceptedRisk, {})
                      .State;
  EXPECT_EQ(A.Findings.front().Status, "selected_partial_evidence");
  EXPECT_NE(markdown(A, R1, 0, 1).find("accepted_risk"), std::string::npos);
  EXPECT_EQ(markdown(A, R0, 0, 1).find("accepted_risk"), std::string::npos);
  EXPECT_NE(markdown(A, R1, 0, 1).find("caller assertions"), std::string::npos);
  fails([&] { (void)markdown(A, R1, 0, 513); }, "invalid_page");
}

TEST_F(WebPackageAssessment, CombinedOwnerRecordsRespectTheRetainedBudget) {
  const auto N = inventory();
  const auto P = analyzePackages(N, N.Artifacts[1].ID, "package-json");
  PackageBehavior B;
  B.ID = identity("assessment-test-behavior", {P.ID});
  B.PackageAnalysisID = P.ID;
  PackageBehaviorSource Source;
  Source.SourceID = identity("assessment-test-source", {P.ID});
  Source.ArtifactID = P.ArtifactID;
  Source.SourceHash = P.BlobHash;
  Source.Status = "partial";
  for (uint64_t I = 0; I < MaxPackageBehaviorRecords; ++I) {
    NodeBehaviorRecord R;
    R.ID = identity("assessment-test-call", {std::to_string(I)});
    R.NodeID = identity("assessment-test-node", {std::to_string(I)});
    R.API = "child_process.exec";
    R.Kind = "process";
    R.Start = I * 8;
    R.End = R.Start + 7;
    Source.Records.push_back(std::move(R));
  }
  B.Sources.push_back(std::move(Source));
  PackageAssessmentSelection S;
  S.Behavior = &B;
  fails([&] { (void)composePackageAssessment(P, N, S); },
        "package_assessment_retained_budget_exceeded");
  // An independent, smaller selection still works after the failed compose.
  EXPECT_EQ(composePackageAssessment(P, N, {}).Findings.size(), 10u);
}
} // namespace

//===- PackageAdvisoryTests.cpp - Offline metadata counterexamples --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent SemVer, OSV coverage, chronology and aggregate-budget fixtures.
///
//===----------------------------------------------------------------------===//

#include "BlobStore.h"
#include "gtest/gtest.h"
#include "packages/advisories/Publication.h"
#include "packages/advisories/SemVer.h"

#include "neverd/web/Error.h"
#include "neverd/web/PackageAdvisories.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
using namespace neverd::web;
using namespace neverd::web::package_advisories;

class WebPackageAdvisories : public ::testing::Test {
protected:
  void SetUp() override {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX blob storage profile required";
#endif
  }
};
using WebPackageEvidence = WebPackageAdvisories;

Artifact artifact(std::string_view Text) {
  BlobStore Store(2 * 1024 * 1024);
  Store.append(Text);
  Store.seal();
  Artifact A;
  A.Content = Store.whole();
  A.BlobHash = A.Content.digest();
  A.ID = identity("advisory-test-artifact", {A.BlobHash});
  return A;
}
PackageEvidenceDescriptor descriptor() {
  return readPackageEvidenceDescriptor(artifact(R"({"schema_version":1,
    "source":"CANARY_SOURCE_URI", "retrieved_at":"2026-10-11T12:00:00Z",
    "as_of":"2026-10-11T11:00:00Z"})"));
}
PackageAnalysis package(std::string Version = "1.0.0",
                        std::string Name = "CANARY_PACKAGE") {
  PackageAnalysis P;
  P.Kind = "package-json";
  P.ID = identity("advisory-test-package", {Version, Name});
  PackageInstance I;
  I.ID = identity("package-instance", {P.ID, "0"});
  I.Name = std::move(Name);
  I.Version = std::move(Version);
  I.NameSource = I.VersionSource = "manifest";
  I.VersionKind = "package_version_declaration";
  P.Packages.push_back(std::move(I));
  return P;
}
PackageAdvisory read(std::string_view Entries, std::string_view Extra = {}) {
  return readPackageAdvisory(
      artifact(
          std::string(R"({"schema_version":"1.9.1","id":"CANARY_ID",
        "modified":"2026-10-10T00:00:00Z","published":"2026-10-09T00:00:00Z")") +
          (Entries.empty() ? "" : ",\"affected\":" + std::string(Entries)) +
          std::string(Extra) + "}"),
      descriptor());
}
PackageAdvisory advisory(std::string_view Selectors) {
  return read(std::string(R"([{"package":{"ecosystem":"npm",
    "name":"CANARY_PACKAGE"},)") +
              std::string(Selectors) + "}]");
}
PackageAdvisoryMatch match(const PackageAdvisory &A,
                           std::string Version = "1.0.0") {
  return matchPackageAdvisories(package(std::move(Version)), {&A})
      .Records.at(0);
}

TEST(WebPackageSemVer, StrictGrammarDoesNotCoercePackageManagerRanges) {
  for (const auto *Text : {"0.0.0", "1.2.3", "1.2.3-0", "1.2.3-alpha.2",
                           "1.2.3+000", "1.2.3-x-y-z.--+build.01"})
    EXPECT_TRUE(parseSemVer(Text)) << Text;
  for (const auto *Text :
       {"",          "1",         "1.2",           "v1.2.3",
        " 1.2.3",    "1.2.3 ",    "^1.2.3",        "~1.2.3",
        "1.x",       "01.2.3",    "1.02.3",        "1.2.03",
        "1.2.3.4",   "1.2.3-01",  "1.2.3-",        "1.2.3-alpha..2",
        "1.2.3+",    "1.2.3+a+b", "1.2.3-alpha_2", "1.2.3+foo.",
        "1.2.3-foo."})
    EXPECT_FALSE(parseSemVer(Text)) << Text;
  EXPECT_FALSE(parseSemVer(std::string(1025, '9')));
}

TEST(WebPackageSemVer, IndependentPrecedenceExamplesAndHugeDecimalComponents) {
  const std::vector<std::string> Ordered{
      "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
      "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
      "1.0.0-rc.1",  "1.0.0",         "2.0.0"};
  for (size_t I = 0; I < Ordered.size(); ++I)
    for (size_t J = 0; J < Ordered.size(); ++J)
      EXPECT_EQ(
          compareSemVer(*parseSemVer(Ordered[I]), *parseSemVer(Ordered[J])),
          I == J  ? 0
          : I < J ? -1
                  : 1);
  EXPECT_EQ(compareSemVer(*parseSemVer("1.2.3+first.000"),
                          *parseSemVer("1.2.3+second.001")),
            0);
  EXPECT_LT(compareSemVer(*parseSemVer(std::string(300, '9') + ".0.0"),
                          *parseSemVer("1" + std::string(300, '0') + ".0.0")),
            0);
  EXPECT_GT(compareSemVer(*parseSemVer("1.0.0-" + std::string(300, '9')),
                          *parseSemVer("1.0.0-999999999999999999999999999999")),
            0);
}

TEST(WebPackageEvidenceTime, ExplicitUTCChronologyNeverUsesTheHostClock) {
  EXPECT_EQ(packageEvidenceTime("2024-02-29T23:59:59.123456789Z").Status,
            "valid_utc_timestamp");
  for (const auto *Text :
       {"2023-02-29T00:00:00Z", "1900-02-29T00:00:00Z", "0000-01-01T00:00:00Z",
        "2026-13-01T00:00:00Z", "2026-10-10T24:00:00Z", "2026-10-10T00:00:60Z",
        "2026-10-10T00:00:00+00:00", "CANARY_TIME"})
    EXPECT_NE(packageEvidenceTime(Text).Status, "valid_utc_timestamp");
  EXPECT_EQ(packageEvidenceTime("2000-02-29T00:00:00Z").Status,
            "valid_utc_timestamp");
  EXPECT_EQ(packageEvidenceTime("2026-10-10T00:00:00.1Z").OrderKey,
            packageEvidenceTime("2026-10-10T00:00:00.100Z").OrderKey);
}

TEST_F(WebPackageEvidence, MissingConflictingAndUnsupportedDescriptors) {
  EXPECT_EQ(descriptor().Status, "declared_source_and_times");
  EXPECT_EQ(
      readPackageEvidenceDescriptor(artifact(R"({"schema_version":1})")).Status,
      "incomplete_source_or_times");
  EXPECT_EQ(readPackageEvidenceDescriptor(artifact(R"({"schema_version":1,
    "source":"CANARY", "as_of":"2026-10-11T00:00:01Z",
    "retrieved_at":"2026-10-11T00:00:00Z"})"))
                .Status,
            "conflicting_declared_times");
  EXPECT_THROW(readPackageEvidenceDescriptor(
                   artifact(R"({"schema_version":1,"execute":false})")),
               Error);
}

TEST_F(WebPackageAdvisories, UnsortedIntervalsAndEndpointRules) {
  const auto A = advisory(R"("ranges":[{"type":"SEMVER","events":[
    {"fixed":"3.2.5"},{"introduced":"3.0.0"},
    {"fixed":"1.0.2"},{"introduced":"1.0.0"}]}])");
  for (const auto *V : {"1.0.0", "1.0.1", "3.0.0", "3.2.4"})
    EXPECT_EQ(match(A, V).Status, "declared_version_match") << V;
  for (const auto *V : {"1.0.0-alpha", "1.0.2", "2.0.0", "3.2.5", "4.0.0"})
    EXPECT_EQ(match(A, V).Status, "version_not_in_admitted_selectors") << V;
  const auto All =
      advisory(R"("ranges":[{"type":"SEMVER","events":[{"introduced":"0"}]}])");
  EXPECT_EQ(match(All, "0.0.0-0").Status, "declared_version_match");
  const auto Last = advisory(R"("ranges":[{"type":"SEMVER","events":[
    {"introduced":"0"},{"last_affected":"1.0.0"}]}])");
  EXPECT_EQ(match(Last).Status, "declared_version_match");
  EXPECT_EQ(match(Last, "1.0.1").Status, "version_not_in_admitted_selectors");
}

TEST_F(WebPackageAdvisories, EnumeratedVersionsAndLimitsAreSeparateUnions) {
  const auto Versions = advisory(R"("versions":["3.0.0"],"ranges":[
    {"type":"SEMVER","events":[{"introduced":"0"},{"fixed":"2.0.0"}]}])");
  EXPECT_EQ(match(Versions, "3.0.0").Status, "declared_version_match");
  const auto Limits = advisory(R"("ranges":[{"type":"SEMVER","events":[
    {"introduced":"0"},{"limit":"2.0.0"},{"limit":"4.0.0"}]}])");
  EXPECT_EQ(match(Limits, "3.0.0").Status, "declared_version_match");
  EXPECT_EQ(match(Limits, "4.0.0").Status, "version_not_in_admitted_selectors");
  const auto Infinite = advisory(R"("ranges":[{"type":"SEMVER","events":[
    {"introduced":"0"},{"limit":"*"},{"limit":"1.0.0"}]}])");
  EXPECT_EQ(match(Infinite, "4.0.0").Status, "declared_version_match");
  const auto Exact = advisory(R"("versions":["1.0.0+buildA"])");
  EXPECT_EQ(match(Exact, "1.0.0+buildB").Status,
            "version_not_in_admitted_selectors");
  EXPECT_EQ(match(Exact, "1.0.0+buildA").Status, "declared_version_match");
}

TEST_F(WebPackageAdvisories, UnsupportedSelectorsPreventNegativeConclusions) {
  for (
      const auto *Events :
      {R"({"type":"ECOSYSTEM","events":[{"introduced":"0"}]})",
       R"({"type":"SEMVER","events":[{"introduced":"v1.0.0"}]})",
       R"({"type":"SEMVER","events":[{"fixed":"1.0.0"}]})",
       R"({"type":"SEMVER","events":[{"introduced":"0"},{"fixed":"2.0.0"},{"last_affected":"3.0.0"}]})",
       R"({"type":"SEMVER","events":[{"introduced":"1.0.0"},{"fixed":"1.0.0+build"}]})"}) {
    const auto A = advisory(std::string(R"("versions":["3.0.0"],"ranges":[)") +
                            Events + "]");
    EXPECT_EQ(match(A).Status, "coverage_unknown");
    const auto Positive = match(A, "3.0.0");
    EXPECT_EQ(Positive.Status, "declared_version_match");
    EXPECT_FALSE(Positive.SelectorCoverageComplete);
  }
}

TEST_F(WebPackageAdvisories, WildcardReservedAndMissingSelectorsStayExplicit) {
  const auto Wild = read(R"([{"package":{"ecosystem":"npm","name":"*"},
    "ranges":[{"type":"SEMVER","events":[{"introduced":"0"}]}]}])");
  EXPECT_EQ(match(Wild).Status, "declared_version_match");
  for (const auto *Entries :
       {"", "[]",
        R"([{"package":{"ecosystem":"npm","name":"CANARY_PACKAGE"}}])",
        R"([{"versions":["1.0.0"]}])"})
    EXPECT_EQ(match(read(Entries)).Status, "coverage_unknown");
  const auto PURL = read(R"([{"package":{"ecosystem":"npm",
    "name":"CANARY_PACKAGE","purl":"pkg:npm/another"},"versions":["1.0.0"]}])");
  EXPECT_EQ(match(PURL).Status, "coverage_unknown");
}

TEST_F(WebPackageAdvisories,
       InstalledAliasesAndConflictingDeclarationsAreNotIdentity) {
  const auto A = advisory(R"("versions":["1.0.0"])");
  auto P = package();
  P.Packages[0].Name = "different";
  P.Packages[0].InstalledName = "CANARY_PACKAGE";
  EXPECT_EQ(matchPackageAdvisories(P, {&A}).Records[0].Status,
            "no_matching_package_declaration");
  P.Packages[0].Name.reset();
  EXPECT_EQ(matchPackageAdvisories(P, {&A}).Records[0].Status,
            "coverage_unknown");
  P = package();
  P.Packages[0].ManifestStatus = "conflicting_declarations";
  EXPECT_EQ(matchPackageAdvisories(P, {&A}).Records[0].Status,
            "coverage_unknown");
  for (const auto *V : {"^1.0.0", "v1.0.0", "1.0", "git+CANARY"})
    EXPECT_EQ(match(A, V).Status, "coverage_unknown");
  EXPECT_TRUE(matchPackageAdvisories(package(), {}).Records.empty());
}

TEST_F(WebPackageAdvisories, OnlyExactVersionlessNpmPurlsConfirmNameAgreement) {
  const auto A = read(R"([{"package":{"ecosystem":"npm","name":"lodash",
    "purl":"pkg:npm/lodash"},"versions":["1.0.0"]}])");
  EXPECT_EQ(A.Affected[0].IdentityStatus, "matching_versionless_npm_purl");
  EXPECT_EQ(matchPackageAdvisories(package("1.0.0", "lodash"), {&A}).MatchCount,
            1u);
  const auto Scoped =
      read(R"([{"package":{"ecosystem":"npm","name":"@Scope/Name",
    "purl":"pkg:npm/%40Scope/Name"},"versions":["1.0.0"]}])");
  EXPECT_EQ(matchPackageAdvisories(package("1.0.0", "@Scope/Name"), {&Scoped})
                .MatchCount,
            1u);
  for (const auto *PURL : {"pkg:npm/another", "pkg:npm/lodash@1.0.0",
                           "pkg:npm/lodash?repository_url=CANARY",
                           "pkg:npm/lodash#CANARY", "pkg:npm/%6codash"}) {
    const auto Input = read(std::string(R"([{"package":{"ecosystem":"npm",
      "name":"lodash","purl":")") +
                            PURL + R"("},"versions":["1.0.0"]}])");
    EXPECT_EQ(matchPackageAdvisories(package("1.0.0", "lodash"), {&Input})
                  .Records[0]
                  .Status,
              "coverage_unknown");
  }
}

TEST_F(WebPackageAdvisories,
       WithdrawalTimeAndSeverityRemainSeparateDeclarations) {
  const auto A =
      read(R"([{"package":{"ecosystem":"npm","name":"CANARY_PACKAGE"},
    "versions":["1.0.0"],"severity":[{"type":"CVSS_V3","score":"CANARY_SCORE"}]}])",
           R"(,"withdrawn":"2026-10-10T00:00:00Z","severity":[
    {"type":"CVSS_V4","score":"CANARY_VECTOR","source":"CANARY_AUTHORITY"}])");
  EXPECT_EQ(A.TemporalStatus, "withdrawn_at_supplied_as_of");
  EXPECT_EQ(A.SeverityStatus, "conflicting_severity_scopes");
  ASSERT_EQ(A.Severities.size(), 2u);
  EXPECT_EQ(A.Severities[0].Validation, "supplied_unvalidated");
  EXPECT_EQ(match(A).Status, "declared_version_match");
  EXPECT_EQ(readPackageAdvisory(artifact(R"({"id":"CANARY_ID",
    "modified":"2026-10-12T00:00:00Z"})"),
                                descriptor())
                .TemporalStatus,
            "advisory_newer_than_supplied_as_of");
  EXPECT_EQ(readPackageAdvisory(artifact(R"({"id":"CANARY_ID",
    "modified":"2026-10-10T00:00:00Z","published":"2026-10-12T00:00:00Z"})"),
                                descriptor())
                .TemporalStatus,
            "conflicting_advisory_times");
}

TEST_F(WebPackageAdvisories, HashStructureOwnersAndBoundsFailExplicitly) {
  auto Input =
      artifact(R"({"id":"CANARY_ID","modified":"2026-10-10T00:00:00Z"})");
  Input.BlobHash = std::string(64, '0');
  EXPECT_THROW(readPackageAdvisory(Input, descriptor()), Error);
  EXPECT_THROW(readPackageAdvisory(artifact("{}"), descriptor()), Error);
  EXPECT_THROW(advisory(R"("ranges":[{"type":"SEMVER","events":[
    {"introduced":"0","fixed":"1.0.0"}]}])"),
               Error);
  auto A = advisory(R"("versions":["1.0.0"])");
  EXPECT_THROW(matchPackageAdvisories(package(), {&A, &A}), Error);
  EXPECT_THROW(matchPackageAdvisories(package(), {nullptr}), Error);
  A.Affected[0].ID = std::string(64, '0');
  EXPECT_THROW(match(A), Error);
  llvm::json::Array Versions;
  for (unsigned I = 0; I < 8193; ++I)
    Versions.push_back("1.0.0");
  const auto Text =
      llvm::formatv("{0}", llvm::json::Value(std::move(Versions))).str();
  EXPECT_THROW(advisory("\"versions\":" + Text), Error);
}

TEST_F(WebPackageAdvisories, AggregateMatchingWorkIsChargedBeforePublication) {
  auto P = package(std::string(1000, '9') + ".0.0");
  P.Packages.resize(4096, P.Packages[0]);
  for (size_t I = 0; I < P.Packages.size(); ++I)
    P.Packages[I].ID = identity("package-instance", {P.ID, std::to_string(I)});
  const auto A = advisory(R"("ranges":[{"type":"SEMVER","events":[
    {"introduced":"0"},{"fixed":"999999999999999999999999999999999999.0.0"}]}])");
  try {
    (void)matchPackageAdvisories(P, {&A});
    FAIL() << "Aggregate comparison budget was not enforced";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), "package_metadata_work_budget_exceeded");
  }
  EXPECT_TRUE(matchPackageAdvisories(P, {}).Records.empty());
}

TEST_F(WebPackageAdvisories, PublicationKeepsPrivateDeclarationsBehindAnchors) {
  const auto A =
      read(R"([{"package":{"ecosystem":"npm","name":"CANARY_PACKAGE"},
    "versions":["1.0.0"]}])",
           R"(,"severity":[{"type":"CANARY_SCHEME",
    "score":"CANARY_SCORE","source":"CANARY_SOURCE"}])");
  const auto M = matchPackageAdvisories(package(), {&A});
  const auto Check = [](llvm::json::Object O) {
    const auto Text =
        llvm::formatv("{0}", llvm::json::Value(std::move(O))).str();
    EXPECT_EQ(Text.find("CANARY"), Text.npos);
  };
  Check(summary(A, 7));
  Check(summary(M, 7));
  Check(record(A.Affected[0]));
  Check(record(A.Severities[0]));
  Check(record(M.Records[0]));
  EXPECT_EQ(summary(M, 7).getBoolean("exploitability_verified"), false);
  EXPECT_EQ(summary(A, 7).getBoolean("freshness_assessed"), false);
}

TEST_F(WebPackageAdvisories, OptionalPinnedPublicOsvRecordRetainsItsBoundary) {
  const auto *Path = std::getenv("NEVERD_OSV_NPM_ADVISORY");
  if (!Path)
    GTEST_SKIP() << "Optional pinned OSV metadata not supplied";
  ASSERT_EQ(std::filesystem::file_size(Path), 4949u);
  std::ifstream Stream(Path, std::ios::binary);
  const std::string Bytes((std::istreambuf_iterator<char>(Stream)), {});
  ASSERT_EQ(sha256(Bytes),
            "a0ed80d486cf363f586b0f031e983c68a136f1f58d1a6e2cc3253dc11319dc75");
  const auto A = readPackageAdvisory(artifact(Bytes), descriptor());
  EXPECT_EQ(A.DeclaredID, "GHSA-p6mc-m468-83gw");
  ASSERT_EQ(A.Affected.size(), 8u);
  EXPECT_EQ(A.TemporalStatus, "declared_times_consistent");
  const auto Before =
      matchPackageAdvisories(package("4.17.18", "lodash"), {&A});
  const auto Fixed = matchPackageAdvisories(package("4.17.19", "lodash"), {&A});
  EXPECT_EQ(Before.Records[0].Status, "declared_version_match");
  EXPECT_EQ(Before.Records[0].MatchedAffectedIDs,
            std::vector<std::string>{A.Affected[0].ID});
  EXPECT_EQ(Fixed.Records[0].Status, "version_not_in_admitted_selectors");
  EXPECT_EQ(summary(Before, 1).getBoolean("exploitability_verified"), false);
}
} // namespace

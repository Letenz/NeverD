//===- PackageReviewTests.cpp - Analyst assertion history boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable evidence, last-request replay and atomic stale/capacity refusal.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/web/Error.h"
#include "neverd/web/PackageAssessment.h"

namespace {
using namespace neverd::web;
PackageAssessment assessment() {
  PackageAssessment A;
  A.ID = identity("test-package-assessment", {"first"});
  PackageFinding F;
  F.ID = identity("test-package-finding", {A.ID, "first"});
  F.Kind = PackageFindingKind::Integrity;
  F.Status = "original_inventory_unbound";
  A.Findings.push_back(F);
  F.ID = identity("test-package-finding", {A.ID, "second"});
  F.Kind = PackageFindingKind::Advisory;
  F.Status = "coverage_unknown";
  A.Findings.push_back(F);
  return A;
}
template <class Action> void fails(Action Run, const char *Code) {
  try {
    Run();
    FAIL() << "Expected refusal";
  } catch (const Error &E) {
    EXPECT_STREQ(E.what(), Code);
  }
}

TEST(WebPackageReview, FixedDispositionsNeverAcceptUnboundedNotes) {
  for (const auto &Name :
       {"unreviewed", "needs_review", "accepted_risk", "false_positive"})
    EXPECT_EQ(packageDispositionName(parsePackageDisposition(Name)), Name);
  for (const auto &Name : {"", "safe", "approved", "CANARY_PRIVATE_NOTE"})
    fails([&] { (void)parsePackageDisposition(Name); },
          "invalid_package_disposition");
  fails(
      [&] {
        (void)packageDispositionName(static_cast<PackageDisposition>(99));
      },
      "invalid_package_disposition");
}

TEST(WebPackageReview, SequentialDecisionsPreserveBaseAndPriorHistory) {
  const auto A = assessment();
  const auto R0 = createPackageReview(A);
  const auto U1 = appendPackageReview(A, R0, R0.HeadID, A.Findings[0].ID,
                                      PackageDisposition::NeedsReview, {});
  EXPECT_FALSE(U1.Replay);
  EXPECT_EQ(U1.AddedEvents, 1u);
  EXPECT_GT(U1.AddedBytes, 0u);
  const auto U2 =
      appendPackageReview(A, U1.State, U1.State.HeadID, A.Findings[1].ID,
                          PackageDisposition::AcceptedRisk, {});
  ASSERT_EQ(U2.State.Decisions.size(), 2u);
  EXPECT_EQ(U2.State.Decisions[0].ID, U1.State.Decisions[0].ID);
  EXPECT_EQ(U2.State.Decisions[1].ParentHead, U1.State.HeadID);
  EXPECT_EQ(U2.State.Decisions[0].ParentHead, R0.RootID);
  EXPECT_EQ(U2.State.Decisions[1].Index, 1u);
  EXPECT_EQ(U2.State.HeadID, U2.State.Decisions[1].ID);
  EXPECT_EQ(U2.State.RetainedBytes, U1.AddedBytes + U2.AddedBytes);
  EXPECT_TRUE(R0.Decisions.empty());
  EXPECT_EQ(U1.State.Decisions.size(), 1u);
  EXPECT_EQ(A.Findings[0].Status, "original_inventory_unbound");
  EXPECT_EQ(A.Findings[1].Status, "coverage_unknown");
}

TEST(WebPackageReview, OnlyTheLastExactRequestReplaysIncludingAtCapacity) {
  const auto A = assessment();
  const auto R0 = createPackageReview(A);
  const auto U1 = appendPackageReview(A, R0, R0.HeadID, A.Findings[0].ID,
                                      PackageDisposition::NeedsReview, {});
  const auto Replay =
      appendPackageReview(A, U1.State, R0.HeadID, A.Findings[0].ID,
                          PackageDisposition::NeedsReview, {0, 0});
  EXPECT_TRUE(Replay.Replay);
  EXPECT_EQ(Replay.State.HeadID, U1.State.HeadID);
  EXPECT_EQ(Replay.State.Decisions.size(), 1u);
  EXPECT_EQ(Replay.AddedEvents, 0u);
  EXPECT_EQ(Replay.AddedBytes, 0u);
  fails(
      [&] {
        (void)appendPackageReview(A, U1.State, R0.HeadID, A.Findings[0].ID,
                                  PackageDisposition::FalsePositive, {});
      },
      "stale_package_review_head");
  const auto U2 =
      appendPackageReview(A, U1.State, U1.State.HeadID, A.Findings[1].ID,
                          PackageDisposition::AcceptedRisk, {});
  fails(
      [&] {
        (void)appendPackageReview(A, U2.State, R0.HeadID, A.Findings[0].ID,
                                  PackageDisposition::NeedsReview, {});
      },
      "stale_package_review_head");
}

TEST(WebPackageReview, SharedRemainingCapacityRefusesWithoutChangingTheInput) {
  const auto A = assessment();
  const auto R0 = createPackageReview(A);
  fails(
      [&] {
        (void)appendPackageReview(A, R0, R0.HeadID, A.Findings[0].ID,
                                  PackageDisposition::NeedsReview, {0, 4096});
      },
      "package_review_decision_budget_exceeded");
  fails(
      [&] {
        (void)appendPackageReview(A, R0, R0.HeadID, A.Findings[0].ID,
                                  PackageDisposition::NeedsReview, {1, 1});
      },
      "package_review_byte_budget_exceeded");
  EXPECT_TRUE(R0.Decisions.empty());
  EXPECT_EQ(R0.HeadID, R0.RootID);
  EXPECT_EQ(R0.RetainedBytes, 0u);
  auto R = R0;
  for (uint64_t I = 0; I < MaxPackageReviewDecisions; ++I) {
    const auto U = appendPackageReview(A, R, R.HeadID, A.Findings[I % 2].ID,
                                       PackageDisposition::NeedsReview, {});
    R = U.State;
  }
  const auto OldHead = R.HeadID;
  fails(
      [&] {
        (void)appendPackageReview(A, R, R.HeadID, A.Findings[0].ID,
                                  PackageDisposition::NeedsReview, {});
      },
      "package_review_decision_budget_exceeded");
  EXPECT_EQ(R.HeadID, OldHead);
  EXPECT_EQ(R.Decisions.size(), MaxPackageReviewDecisions);
  EXPECT_LE(R.RetainedBytes, MaxPackageReviewBytes);
}

TEST(WebPackageReview, ForeignFindingOwnerAndAlteredEarlierHistoryAreRefused) {
  const auto A = assessment();
  const auto R0 = createPackageReview(A);
  fails(
      [&] {
        (void)appendPackageReview(A, R0, R0.HeadID, std::string(64, '0'),
                                  PackageDisposition::NeedsReview, {});
      },
      "unknown_package_assessment_finding");
  auto Other = A;
  Other.ID = identity("test-package-assessment", {"other"});
  fails(
      [&] {
        (void)appendPackageReview(Other, R0, R0.HeadID, A.Findings[0].ID,
                                  PackageDisposition::NeedsReview, {});
      },
      "package_review_owner_mismatch");
  auto R1 = appendPackageReview(A, R0, R0.HeadID, A.Findings[0].ID,
                                PackageDisposition::NeedsReview, {})
                .State;
  auto R2 = appendPackageReview(A, R1, R1.HeadID, A.Findings[1].ID,
                                PackageDisposition::AcceptedRisk, {})
                .State;
  R2.Decisions[0].Disposition = PackageDisposition::FalsePositive;
  fails(
      [&] {
        (void)appendPackageReview(A, R2, R2.HeadID, A.Findings[0].ID,
                                  PackageDisposition::Unreviewed, {});
      },
      "package_review_chain_mismatch");
  // Even a correctly hashed replacement cannot add a foreign finding to the
  // retained history. The complete chain must belong to this assessment.
  auto Forged = R1;
  auto &D = Forged.Decisions[0];
  D.FindingID = identity("foreign-package-finding", {"other"});
  D.ID = identity("package-review-decision",
                  {PackageAssessmentProfile, D.AssessmentID, D.ParentHead,
                   D.FindingID, "needs_review", "0"});
  Forged.HeadID = D.ID;
  fails([&] { validatePackageReview(A, Forged); },
        "package_review_chain_mismatch");
}
} // namespace

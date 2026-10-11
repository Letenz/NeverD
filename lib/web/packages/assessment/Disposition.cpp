//===- Disposition.cpp - Immutable analyst decision transitions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compare-and-append decisions without mutating analysis or inventing review.
///
//===----------------------------------------------------------------------===//

#include "../evidence/Admission.h"

#include "neverd/web/PackageAssessment.h"

#include <algorithm>
#include <set>

namespace neverd::web {
namespace {
// Each row has four bounded identities, fixed disposition and ordinal. This
// conservative charge includes strings, vector storage and publication data.
constexpr uint64_t DecisionBytes = 1024;
std::string rootID(const PackageAssessment &A) {
  package_evidence::requireIdentity(A.ID);
  if (A.Findings.size() > MaxPackageAssessmentFindings)
    throw Error("package_assessment_finding_budget_exceeded");
  return identity("package-review-root", {PackageAssessmentProfile, A.ID});
}
std::string decisionID(const PackageReviewDecision &D) {
  return identity("package-review-decision",
                  {PackageAssessmentProfile, D.AssessmentID, D.ParentHead,
                   D.FindingID, packageDispositionName(D.Disposition),
                   std::to_string(D.Index)});
}
} // namespace

PackageDisposition parsePackageDisposition(std::string_view Value) {
  if (Value == "unreviewed")
    return PackageDisposition::Unreviewed;
  if (Value == "needs_review")
    return PackageDisposition::NeedsReview;
  if (Value == "accepted_risk")
    return PackageDisposition::AcceptedRisk;
  if (Value == "false_positive")
    return PackageDisposition::FalsePositive;
  throw Error("invalid_package_disposition");
}
std::string_view packageDispositionName(PackageDisposition Value) {
  switch (Value) {
  case PackageDisposition::Unreviewed:
    return "unreviewed";
  case PackageDisposition::NeedsReview:
    return "needs_review";
  case PackageDisposition::AcceptedRisk:
    return "accepted_risk";
  case PackageDisposition::FalsePositive:
    return "false_positive";
  }
  throw Error("invalid_package_disposition");
}
PackageReviewState createPackageReview(const PackageAssessment &A) {
  PackageReviewState R;
  R.AssessmentID = A.ID;
  R.RootID = rootID(A);
  R.HeadID = R.RootID;
  return R;
}

void validatePackageReview(const PackageAssessment &A,
                           const PackageReviewState &Prior) {
  const auto Root = rootID(A);
  if (Prior.AssessmentID != A.ID || Prior.RootID != Root ||
      Prior.Decisions.size() > MaxPackageReviewDecisions ||
      Prior.RetainedBytes != Prior.Decisions.size() * DecisionBytes)
    throw Error("package_review_owner_mismatch");
  std::set<std::string_view> Findings;
  for (const auto &F : A.Findings) {
    package_evidence::requireIdentity(F.ID);
    if (!Findings.insert(F.ID).second)
      throw Error("package_review_owner_mismatch");
  }
  // Existing decisions are retained immutable owner records. Validate the
  // whole bounded chain before preparing a replacement, not only its tail.
  std::string Head = Root;
  for (size_t I = 0; I < Prior.Decisions.size(); ++I) {
    const auto &D = Prior.Decisions[I];
    package_evidence::requireIdentity(D.FindingID);
    if (!Findings.contains(D.FindingID) || D.AssessmentID != A.ID ||
        D.Index != I || D.ParentHead != Head || D.ID != decisionID(D))
      throw Error("package_review_chain_mismatch");
    Head = D.ID;
  }
  if (Head != Prior.HeadID)
    throw Error("package_review_chain_mismatch");
}

PackageReviewUpdate appendPackageReview(const PackageAssessment &A,
                                        const PackageReviewState &Prior,
                                        std::string_view ExpectedHead,
                                        std::string_view FindingID,
                                        PackageDisposition Disposition,
                                        PackageReviewCapacity Remaining) {
  package_evidence::requireIdentity(ExpectedHead);
  package_evidence::requireIdentity(FindingID);
  (void)packageDispositionName(Disposition);
  if (Remaining.Events > MaxPackageReviewDecisions ||
      Remaining.Bytes > MaxPackageReviewBytes)
    throw Error("invalid_package_review_capacity");
  validatePackageReview(A, Prior);
  if (std::none_of(A.Findings.begin(), A.Findings.end(),
                   [&](const auto &F) { return F.ID == FindingID; }))
    throw Error("unknown_package_assessment_finding");
  if (!Prior.Decisions.empty()) {
    const auto &Last = Prior.Decisions.back();
    if (Last.ParentHead == ExpectedHead && Last.FindingID == FindingID &&
        Last.Disposition == Disposition)
      return PackageReviewUpdate{Prior, 0, 0, true};
  }
  if (ExpectedHead != Prior.HeadID)
    throw Error("stale_package_review_head");
  if (!Remaining.Events || Prior.Decisions.size() == MaxPackageReviewDecisions)
    throw Error("package_review_decision_budget_exceeded");
  if (Remaining.Bytes < DecisionBytes ||
      DecisionBytes > MaxPackageReviewBytes - Prior.RetainedBytes)
    throw Error("package_review_byte_budget_exceeded");
  PackageReviewDecision D;
  D.AssessmentID = A.ID;
  D.FindingID = FindingID;
  D.ParentHead = Prior.HeadID;
  D.Disposition = Disposition;
  D.Index = Prior.Decisions.size();
  D.ID = decisionID(D);
  PackageReviewUpdate Update{Prior, 1, DecisionBytes, false};
  Update.State.HeadID = D.ID;
  Update.State.Decisions.push_back(std::move(D));
  Update.State.RetainedBytes += DecisionBytes;
  return Update;
}
} // namespace neverd::web

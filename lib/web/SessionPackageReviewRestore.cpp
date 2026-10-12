//===- SessionPackageReviewRestore.cpp - Atomic offline review import ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parse only a bounded fixed journal schema and delegate all chain, finding,
/// prefix and capacity decisions to the authoritative review owner.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"
#include "packages/assessment/Publication.h"
#include "packages/evidence/Admission.h"

namespace neverd::web {
namespace {
void fields(const llvm::json::Object &O,
            std::initializer_list<llvm::StringRef> Allowed) {
  if (O.size() != Allowed.size())
    throw Error("invalid_package_review_journal_field");
  for (const auto Field : Allowed)
    if (!O.get(Field))
      throw Error("invalid_package_review_journal_field");
}
std::string required(const llvm::json::Object &O, llvm::StringRef Field) {
  const auto Value = O.getString(Field);
  if (!Value)
    throw Error("invalid_package_review_journal_field");
  return Value->str();
}
PackageReviewState journal(std::string_view Text) {
  auto V = parseBoundedJSON(Text, {MaxPackageReviewBytes, 4, 4096, 64});
  const auto *O = V.getAsObject();
  if (!O)
    throw Error("invalid_package_review_journal");
  fields(*O, {"schema_version", "profile", "package_assessment_id",
              "review_root_id", "review_head_id", "decisions"});
  if (O->getInteger("schema_version") != 1 ||
      required(*O, "profile") != PackageReviewJournalProfile)
    throw Error("unsupported_package_review_journal");
  PackageReviewState Candidate;
  Candidate.AssessmentID = required(*O, "package_assessment_id");
  Candidate.RootID = required(*O, "review_root_id");
  Candidate.HeadID = required(*O, "review_head_id");
  const auto *Rows = O->getArray("decisions");
  if (!Rows || Rows->size() > MaxPackageReviewDecisions)
    throw Error("package_review_decision_budget_exceeded");
  for (const auto &Row : *Rows) {
    const auto *D = Row.getAsObject();
    if (!D)
      throw Error("invalid_package_review_journal");
    fields(*D, {"decision_id", "package_assessment_id", "finding_id",
                "parent_head_id", "disposition", "decision_index",
                "assertion_source"});
    const auto Index = D->getInteger("decision_index");
    if (!Index || *Index < 0 || required(*D, "assertion_source") != "caller")
      throw Error("invalid_package_review_journal");
    Candidate.Decisions.push_back(
        {required(*D, "decision_id"), required(*D, "package_assessment_id"),
         required(*D, "finding_id"), required(*D, "parent_head_id"),
         parsePackageDisposition(required(*D, "disposition")),
         uint64_t(*Index)});
  }
  return Candidate;
}
} // namespace

std::string Session::restorePackageReview(std::string_view Revision,
                                          std::string_view ID,
                                          std::string_view ExpectedHead,
                                          std::string_view JournalJSON) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  package_evidence::requireIdentity(ID);
  const auto Found = State->PackageAssessments.find(std::string(ID));
  if (Found == State->PackageAssessments.end())
    throw Error("unknown_package_assessment");
  const auto &A = Found->second;
  auto &R = State->PackageReviews.at(A.ID);
  auto U = web::restorePackageReview(
      A, R, ExpectedHead, journal(JournalJSON),
      {MaxPackageReviewDecisions - State->PackageReviewEvents,
       MaxPackageReviewBytes - State->PackageReviewBytes});
  auto Result = package_assessment::summary(A, U.State, State->Revision);
  Result["restored_events"] = U.AddedEvents;
  Result["replayed"] = U.Replay;
  Result["journal_profile"] = std::string(PackageReviewJournalProfile);
  auto Reply = json(std::move(Result));
  R = std::move(U.State);
  State->PackageReviewEvents += U.AddedEvents;
  State->PackageReviewBytes += U.AddedBytes;
  return Reply;
}
} // namespace neverd::web

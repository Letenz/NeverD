//===- SessionPackageAssessments.cpp - Revision-bound package reviews ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Select retained owner models and publish assessment/decision state
/// atomically.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"
#include "packages/assessment/Publication.h"
#include "packages/evidence/Admission.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
namespace {
template <class Rows>
const auto &selected(const Rows &Cache, std::string_view ID, const char *Code) {
  package_evidence::requireIdentity(ID);
  const auto Found = Cache.find(std::string(ID));
  if (Found == Cache.end())
    throw Error(Code);
  return Found->second;
}
template <class Cache>
auto selection(const llvm::json::Object &O, llvm::StringRef Field,
               const Cache &Rows, uint64_t Maximum, const char *Code) {
  using Row = typename Cache::mapped_type;
  std::vector<const Row *> Result;
  const auto *V = O.get(Field);
  if (!V)
    return Result;
  const auto *Items = V->getAsArray();
  if (!Items || Items->size() > Maximum)
    throw Error("package_assessment_selection_budget_exceeded");
  for (const auto &Item : *Items) {
    const auto ID = Item.getAsString();
    if (!ID)
      throw Error("invalid_package_assessment_selection");
    Result.push_back(&selected(Rows, *ID, Code));
  }
  return Result;
}
template <class Cache>
const typename Cache::mapped_type *
optional(const llvm::json::Object &O, llvm::StringRef Field, const Cache &Rows,
         const char *Code) {
  const auto *V = O.get(Field);
  if (!V)
    return nullptr;
  const auto ID = V->getAsString();
  if (!ID)
    throw Error("invalid_package_assessment_selection");
  return &selected(Rows, *ID, Code);
}
llvm::json::Object options(std::string_view Text) {
  if (Text.empty())
    return {};
  auto V = parseBoundedJSON(Text, {8192, 4, 128, 64});
  auto *O = V.getAsObject();
  if (!O)
    throw Error("invalid_package_assessment_selection");
  if (O->getInteger("schema_version") != 1)
    throw Error("unsupported_package_assessment_schema");
  constexpr std::string_view Fields[] = {
      "schema_version",      "integrity_ids",
      "advisory_ids",        "advisory_matches_id",
      "attestation_ids",     "attestation_digest_match_ids",
      "package_behavior_id", "package_install_id",
      "package_diff_id",     "package_behavior_diff_id"};
  for (const auto &[Key, Value] : *O) {
    const llvm::StringRef Name(Key);
    if (std::find(std::begin(Fields), std::end(Fields),
                  std::string_view(Name.data(), Name.size())) ==
        std::end(Fields))
      throw Error("invalid_package_assessment_field");
  }
  return std::move(*O);
}
void head(const PackageReviewState &R, std::string_view Expected) {
  package_evidence::requireIdentity(Expected);
  if (R.HeadID != Expected)
    throw Error("stale_package_review_head");
}
uint64_t end(uint64_t Size, uint64_t Offset, uint64_t Limit) {
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > Size)
    throw Error("invalid_page");
  return std::min<uint64_t>(Size, Offset + Limit);
}
void continuation(llvm::json::Object &O, uint64_t Offset, uint64_t End,
                  uint64_t Size) {
  O["offset"] = Offset;
  O["next_offset"] =
      End == Size ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  O["page_complete"] = End == Size;
}
} // namespace

std::string Session::assessPackage(std::string_view Revision,
                                   std::string_view PackageID,
                                   std::string_view SelectionJSON) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto &P =
      selected(State->PackageAnalyses, PackageID, "unknown_package_analysis");
  const auto &Namespace = selected(State->PackageNamespaces, PackageID,
                                   "unknown_package_namespace");
  const auto O = options(SelectionJSON);
  auto Integrity = selection(O, "integrity_ids", State->PackageIntegrity, 16,
                             "unknown_package_integrity");
  auto Advisories =
      selection(O, "advisory_ids", State->PackageAdvisories,
                MaxPackageAdvisoryImports, "unknown_package_advisory");
  auto Attestations =
      selection(O, "attestation_ids", State->PackageAttestations,
                MaxPackageAttestationImports, "unknown_package_attestation");
  auto Digests = selection(O, "attestation_digest_match_ids",
                           State->PackageAttestationDigests,
                           MaxPackageAttestationDigestMatches,
                           "unknown_package_attestation_digest_match");
  PackageAssessmentSelection S;
  S.Originals = &State->Published;
  std::vector<const PackageArchive *> Archives;
  for (const auto &[ID, A] : State->PackageArchives)
    Archives.push_back(&A);
  S.Archives = Archives;
  S.AdvisoryMatches =
      optional(O, "advisory_matches_id", State->PackageAdvisoryMatchResults,
               "unknown_package_advisory_matches");
  if (S.AdvisoryMatches && !O.get("advisory_ids"))
    for (const auto &ID : S.AdvisoryMatches->AdvisoryIDs)
      Advisories.push_back(
          &selected(State->PackageAdvisories, ID, "unknown_package_advisory"));
  S.Integrity = Integrity;
  S.Advisories = Advisories;
  S.Attestations = Attestations;
  S.DigestMatches = Digests;
  S.Behavior = optional(O, "package_behavior_id", State->PackageBehaviors,
                        "unknown_package_behavior");
  S.Installation = optional(O, "package_install_id", State->PackageInstalls,
                            "unknown_package_install");
  S.Comparison = optional(O, "package_diff_id", State->PackageDiffs,
                          "unknown_package_diff");
  S.BehaviorComparison =
      optional(O, "package_behavior_diff_id", State->PackageBehaviorDiffs,
               "unknown_package_behavior_diff");
  auto A = composePackageAssessment(P, Namespace, S);
  if (const auto Existing = State->PackageAssessments.find(A.ID);
      Existing != State->PackageAssessments.end())
    return json(package_assessment::summary(
        Existing->second, State->PackageReviews.at(A.ID), State->Revision));
  if (State->PackageAssessments.size() >= MaxCachedPackageAssessments)
    throw Error("package_assessment_cache_budget_exceeded");
  auto R = createPackageReview(A);
  auto Reply = json(package_assessment::summary(A, R, State->Revision));
  const auto ID = A.ID;
  bool AddedAssessment = false;
  try {
    AddedAssessment =
        State->PackageAssessments.emplace(ID, std::move(A)).second;
    if (!AddedAssessment ||
        !State->PackageReviews.emplace(ID, std::move(R)).second)
      throw Error("package_assessment_identity_conflict");
  } catch (...) {
    if (AddedAssessment)
      State->PackageAssessments.erase(ID);
    throw;
  }
  return Reply;
}

std::string Session::packageAssessmentFindings(std::string_view Revision,
                                               std::string_view ID,
                                               std::string_view ReviewHead,
                                               uint64_t Offset,
                                               uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto &A =
      selected(State->PackageAssessments, ID, "unknown_package_assessment");
  const auto &R = State->PackageReviews.at(A.ID);
  head(R, ReviewHead);
  const auto End = end(A.Findings.size(), Offset, Limit);
  const auto Effective = package_assessment::dispositions(A, R);
  auto Reply = package_assessment::summary(A, R, State->Revision);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto D = Effective.find(A.Findings[I].ID);
    Items.emplace_back(package_assessment::record(
        A.Findings[I], D == Effective.end()
                           ? std::optional<PackageDisposition>{}
                           : std::optional<PackageDisposition>{D->second}));
  }
  Reply["items"] = std::move(Items);
  continuation(Reply, Offset, End, A.Findings.size());
  return json(std::move(Reply));
}

std::string Session::reviewPackageFinding(std::string_view Revision,
                                          std::string_view ID,
                                          std::string_view ExpectedHead,
                                          std::string_view FindingID,
                                          std::string_view Disposition) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto &A =
      selected(State->PackageAssessments, ID, "unknown_package_assessment");
  auto &R = State->PackageReviews.at(A.ID);
  auto U = appendPackageReview(
      A, R, ExpectedHead, FindingID, parsePackageDisposition(Disposition),
      {MaxPackageReviewDecisions - State->PackageReviewEvents,
       MaxPackageReviewBytes - State->PackageReviewBytes});
  auto Result = package_assessment::summary(A, U.State, State->Revision);
  Result["replayed"] = U.Replay;
  Result["decision"] = package_assessment::record(U.State.Decisions.back());
  auto Reply = json(std::move(Result));
  R = std::move(U.State);
  State->PackageReviewEvents += U.AddedEvents;
  State->PackageReviewBytes += U.AddedBytes;
  return Reply;
}

std::string Session::packageReviewDecisions(std::string_view Revision,
                                            std::string_view ID,
                                            std::string_view ReviewHead,
                                            uint64_t Offset,
                                            uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto &A =
      selected(State->PackageAssessments, ID, "unknown_package_assessment");
  const auto &R = State->PackageReviews.at(A.ID);
  head(R, ReviewHead);
  const auto End = end(R.Decisions.size(), Offset, Limit);
  auto Reply = package_assessment::summary(A, R, State->Revision);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(package_assessment::record(R.Decisions[I]));
  Reply["items"] = std::move(Items);
  continuation(Reply, Offset, End, R.Decisions.size());
  return json(std::move(Reply));
}

std::string Session::packageAssessmentReport(std::string_view Revision,
                                             std::string_view ID,
                                             std::string_view ReviewHead,
                                             uint64_t Offset,
                                             uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto &A =
      selected(State->PackageAssessments, ID, "unknown_package_assessment");
  const auto &R = State->PackageReviews.at(A.ID);
  head(R, ReviewHead);
  const auto End = end(A.Findings.size(), Offset, Limit);
  auto Reply = package_assessment::summary(A, R, State->Revision);
  Reply["report_format"] = "markdown";
  Reply["report_markdown"] = package_assessment::markdown(A, R, Offset, Limit);
  continuation(Reply, Offset, End, A.Findings.size());
  return json(std::move(Reply));
}
} // namespace neverd::web

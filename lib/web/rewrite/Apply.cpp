//===- Apply.cpp - Exact local rewrite application and reversal ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Apply only recorded ranges and validate the exact candidate before undo.
///
//===----------------------------------------------------------------------===//

#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <algorithm>

namespace neverd::web {
namespace {
std::vector<size_t> selected(const SourceRewrite &A, uint64_t &Steps) {
  if (A.Records.size() > MaxSourceRewriteRecords)
    throw Error("source_rewrite_record_budget_exceeded");
  std::vector<size_t> Order;
  for (size_t I = 0; I < A.Records.size(); ++I) {
    rewrite::charge(Steps);
    if (A.Records[I].Status == "accepted")
      Order.push_back(I);
  }
  std::sort(Order.begin(), Order.end(), [&](auto X, auto Y) {
    rewrite::charge(Steps);
    return A.Records[X].SourceStart < A.Records[Y].SourceStart;
  });
  if (Order.size() != A.Accepted)
    throw Error("source_rewrite_invalid_receipt");
  return Order;
}
} // namespace

void rewrite::apply(SourceRewrite &A, std::string_view Original) {
  const auto Order = selected(A, A.Steps);
  uint64_t Cursor = 0;
  for (const auto I : Order) {
    auto &R = A.Records[I];
    if (R.SourceStart < Cursor || R.SourceStart >= R.SourceEnd ||
        R.SourceEnd > Original.size() || R.After.empty() ||
        Original.substr(R.SourceStart, R.SourceEnd - R.SourceStart) != R.Before)
      throw Error("source_rewrite_invalid_receipt");
    charge(A.Steps, R.Before.size());
    append(A.Text, Original.substr(Cursor, R.SourceStart - Cursor), A.Steps);
    R.Start = A.Text.size();
    append(A.Text, R.After, A.Steps);
    R.End = A.Text.size();
    Cursor = R.SourceEnd;
  }
  append(A.Text, Original.substr(Cursor), A.Steps);
  charge(A.Steps, A.Text.size());
  A.CandidateHash = sha256(A.Text);
}

std::string undoSourceRewrite(const SourceRewrite &A,
                              std::string_view Candidate) {
  const auto Profile = sourceRewriteProfile(A.Pass);
  if ((A.Status != "verified_local_rewrites" &&
       A.Status != "no_eligible_rewrites") ||
      A.SourceSize > MaxSourceRewriteBytes ||
      Candidate.size() > MaxSourceRewriteBytes || Profile.empty() ||
      A.ID != identity("source-rewrite", {A.SourceID, Profile, A.Pass,
                                          A.BindingID, A.ValueID, A.EffectID}))
    throw Error("source_rewrite_invalid_receipt");
  uint64_t Steps = 0;
  rewrite::charge(Steps, Candidate.size());
  if (sha256(Candidate) != A.CandidateHash)
    throw Error("source_rewrite_candidate_mismatch");
  const auto Order = selected(A, Steps);
  std::string Original;
  uint64_t Cursor = 0, SourceCursor = 0;
  for (const auto I : Order) {
    const auto &R = A.Records[I];
    const bool Rename = A.Pass == "scope_renaming";
    if (Rename && (R.BindingID.empty() ||
                   (R.Role != "declaration" && R.Role != "reference")))
      throw Error("source_rewrite_invalid_receipt");
    auto RecordID =
        Rename ? identity("source-rename-record", {A.ID, R.BindingID, R.NodeID,
                                                   R.Role, R.Status, R.Reason})
               : identity("source-rewrite-record",
                          {A.ID, R.NodeID, R.Status, R.Reason});
    if (A.Pass == "boolean_branches") {
      if (R.ConditionID.empty() || R.RetainedID.empty() ||
          R.Condition == NoSourceIndex || R.Retained == NoSourceIndex ||
          (R.Selection != "consequent" && R.Selection != "alternate" &&
           R.Selection != "left" && R.Selection != "right"))
        throw Error("source_rewrite_invalid_receipt");
      RecordID = rewrite::branchRecordIdentity(A, R);
    }
    if (R.ID != RecordID || R.SourceStart < SourceCursor ||
        R.SourceStart >= R.SourceEnd || R.SourceEnd > A.SourceSize ||
        R.SourceEnd - R.SourceStart != R.Before.size() || R.Start < Cursor ||
        R.Start >= R.End || R.End > Candidate.size() ||
        R.End - R.Start != R.After.size() ||
        R.Start - Cursor != R.SourceStart - SourceCursor)
      throw Error("source_rewrite_invalid_receipt");
    rewrite::charge(Steps, R.After.size());
    if (Candidate.substr(R.Start, R.After.size()) != R.After)
      throw Error("source_rewrite_candidate_mismatch");
    rewrite::append(Original, Candidate.substr(Cursor, R.Start - Cursor),
                    Steps);
    rewrite::append(Original, R.Before, Steps);
    Cursor = R.End;
    SourceCursor = R.SourceEnd;
  }
  if (Candidate.size() - Cursor != A.SourceSize - SourceCursor)
    throw Error("source_rewrite_invalid_receipt");
  rewrite::append(Original, Candidate.substr(Cursor), Steps);
  rewrite::charge(Steps, Original.size());
  if (Original.size() != A.SourceSize || sha256(Original) != A.SourceHash)
    throw Error("source_rewrite_inverse_mismatch");
  return Original;
}

} // namespace neverd::web

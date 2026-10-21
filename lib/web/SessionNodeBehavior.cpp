//===- SessionNodeBehavior.cpp - Selected source behavior publication ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cache native source evidence and publish bounded metadata pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"
#include "packages/behavior/Publication.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {

std::string Session::analyzeNodeBehavior(std::string_view Revision,
                                         std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Source = State->Sources.find(std::string(SourceID));
  if (Source == State->Sources.end())
    throw Error("unknown_source");
  const auto Existing = State->NodeBehaviors.find(std::string(SourceID));
  if (Existing != State->NodeBehaviors.end())
    return json(node_behavior::summary(Existing->second, State->Revision));
  if (State->NodeBehaviors.size() >= MaxCachedNodeBehaviors)
    throw Error("node_behavior_cache_budget_exceeded");
  auto A = web::analyzeNodeBehavior(Source->second);
  auto Reply = json(node_behavior::summary(A, State->Revision));
  State->NodeBehaviors.emplace(std::string(SourceID), std::move(A));
  return Reply;
#endif
}

std::string Session::nodeBehaviorRecords(std::string_view Revision,
                                         std::string_view BehaviorID,
                                         uint64_t Offset,
                                         uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const NodeBehavior *Found = nullptr;
  for (const auto &[SourceID, A] : State->NodeBehaviors)
    if (A.ID == BehaviorID)
      Found = &A;
  if (!Found)
    throw Error("unknown_node_behavior");
  const auto &A = *Found;
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > A.Records.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(A.Records.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(
        node_behavior::record(A.Records[I], A.SourceID, A.ArtifactID));
  auto Reply = node_behavior::summary(A, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["next_offset"] = End == A.Records.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End);
  Reply["page_complete"] = End == A.Records.size();
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web

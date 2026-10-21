//===- SessionPackageBehaviorDiff.cpp - Retained behavior comparison ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound optional source selections and atomic comparison paging.
///
//===----------------------------------------------------------------------===//

#include "JsonReader.h"
#include "SessionInternal.h"
#include "packages/behavior/DiffPublication.h"

#include "neverd/web/Limits.h"

#include <algorithm>
#include <array>

namespace neverd::web {
namespace {
std::array<std::string, 2> selections(std::string_view Options) {
  const auto V = parseBoundedJSON(Options, {4096, 4, 64, 64});
  const auto *O = V.getAsObject();
  if (!O || O->getInteger("schema_version") != 1 || O->size() > 3)
    throw Error("invalid_package_behavior_diff_options");
  for (const auto &[Key, Value] : *O)
    if (Key != "schema_version" && Key != "before_package_behavior_id" &&
        Key != "after_package_behavior_id")
      throw Error("invalid_package_behavior_diff_options");
  std::array<std::string, 2> IDs;
  unsigned I = 0;
  for (const auto *Key :
       {"before_package_behavior_id", "after_package_behavior_id"}) {
    if (O->get(Key)) {
      const auto ID = O->getString(Key);
      if (!ID || ID->size() != 64 ||
          ID->find_first_not_of("0123456789abcdef") != ID->npos)
        throw Error("invalid_package_behavior_diff_options");
      IDs[I] = ID->str();
    }
    ++I;
  }
  return IDs;
}
} // namespace

std::string Session::comparePackageBehavior(std::string_view Revision,
                                            std::string_view DiffID,
                                            std::string_view Options) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto IDs = selections(Options);
  const auto Found = State->PackageDiffs.find(std::string(DiffID));
  if (Found == State->PackageDiffs.end())
    throw Error("unknown_package_diff");
  const auto &Base = Found->second;
  auto Select = [&](const std::string &ID,
                    const std::string &PackageID) -> const PackageBehavior * {
    if (ID.empty())
      return nullptr;
    const auto B = State->PackageBehaviors.find(ID);
    if (B == State->PackageBehaviors.end())
      throw Error("unknown_package_behavior");
    if (B->second.PackageAnalysisID != PackageID)
      throw Error("package_behavior_diff_selection_mismatch");
    return &B->second;
  };
  const auto *Before = Select(IDs[0], Base.BeforeID);
  const auto *After = Select(IDs[1], Base.AfterID);
  for (const auto &[ID, D] : State->PackageBehaviorDiffs)
    if (D.PackageDiffID == DiffID && D.BeforeBehaviorID == IDs[0] &&
        D.AfterBehaviorID == IDs[1])
      return json(package_behavior_diff::summary(D, State->Revision));
  if (State->PackageBehaviorDiffs.size() >= MaxCachedPackageBehaviorDiffs)
    throw Error("package_behavior_diff_cache_budget_exceeded");
  auto D = web::comparePackageBehavior(
      Base, State->PackageAnalyses.at(Base.BeforeID),
      State->PackageAnalyses.at(Base.AfterID), Before, After);
  auto Reply = json(package_behavior_diff::summary(D, State->Revision));
  const auto ID = D.ID;
  State->PackageBehaviorDiffs.emplace(ID, std::move(D));
  return Reply;
#endif
}

std::string Session::packageBehaviorDiffRecords(std::string_view Revision,
                                                std::string_view DiffID,
                                                std::string_view Kind,
                                                uint64_t Offset,
                                                uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageBehaviorDiffs.find(std::string(DiffID));
  if (Found == State->PackageBehaviorDiffs.end())
    throw Error("unknown_package_behavior_diff");
  const auto &D = Found->second;
  const auto Total = Kind == "pairs"      ? D.Pairs.size()
                     : Kind == "patterns" ? D.Patterns.size()
                     : Kind == "occurrences"
                         ? D.Occurrences.size()
                         : throw Error("invalid_record_kind");
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > Total)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Total, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "pairs")
      Items.emplace_back(package_behavior_diff::record(D.Pairs[I]));
    else if (Kind == "patterns")
      Items.emplace_back(package_behavior_diff::record(D.Patterns[I]));
    else
      Items.emplace_back(package_behavior_diff::record(D.Occurrences[I]));
  }
  auto Reply = package_behavior_diff::summary(D, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["record_kind"] = std::string(Kind);
  Reply["offset"] = Offset;
  Reply["next_offset"] =
      End == Total ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  Reply["page_complete"] = End == Total;
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web

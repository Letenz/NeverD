//===- SessionPackageInstall.cpp - Retained installation trace selection -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable package namespace selection, bounded cache and metadata paging.
///
//===----------------------------------------------------------------------===//

#include "JsonReader.h"
#include "SessionInternal.h"
#include "packages/install/Internal.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
namespace {
std::string behaviorID(std::string_view Options) {
  const auto V = parseBoundedJSON(Options, {4096, 4, 64, 64});
  const auto *O = V.getAsObject();
  if (!O || O->getInteger("schema_version") != 1 ||
      (O->size() != 1 && O->size() != 2))
    throw Error("invalid_package_install_options");
  if (O->size() == 1)
    return {};
  const auto ID = O->getString("package_behavior_id");
  if (!ID || ID->size() != 64 ||
      ID->find_first_not_of("0123456789abcdef") != ID->npos)
    throw Error("invalid_package_install_options");
  return ID->str();
}
} // namespace

std::string Session::analyzePackageInstall(std::string_view Revision,
                                           std::string_view PackageID,
                                           std::string_view Options) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto ID = behaviorID(Options);
  const auto P = State->PackageAnalyses.find(std::string(PackageID));
  if (P == State->PackageAnalyses.end())
    throw Error("unknown_package_analysis");
  const PackageBehavior *Behavior = nullptr;
  if (!ID.empty()) {
    const auto B = State->PackageBehaviors.find(ID);
    if (B == State->PackageBehaviors.end())
      throw Error("unknown_package_behavior");
    if (B->second.PackageAnalysisID != PackageID)
      throw Error("package_install_selection_mismatch");
    Behavior = &B->second;
  }
  for (const auto &[Key, A] : State->PackageInstalls)
    if (A.PackageAnalysisID == PackageID && A.PackageBehaviorID == ID)
      return json(package_install::summary(A, State->Revision));
  if (State->PackageInstalls.size() >= MaxCachedPackageInstalls)
    throw Error("package_install_cache_budget_exceeded");
  std::vector<PackageInstallInput> Sources;
  if (Behavior)
    for (const auto &B : Behavior->Sources) {
      const auto S = State->Sources.find(B.SourceID);
      if (S == State->Sources.end())
        throw Error("unknown_source");
      const auto View = State->artifactView(B.ArtifactID);
      if (!View)
        throw Error("unknown_artifact");
      Sources.push_back({&S->second, View->Content.size()});
    }
  auto A = web::analyzePackageInstall(
      P->second, State->PackageNamespaces.at(P->first), Behavior, Sources);
  auto Reply = json(package_install::summary(A, State->Revision));
  const auto Key = A.ID;
  State->PackageInstalls.emplace(Key, std::move(A));
  return Reply;
#endif
}

std::string Session::packageInstallRecords(std::string_view Revision,
                                           std::string_view InstallID,
                                           std::string_view Kind,
                                           uint64_t Offset,
                                           uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageInstalls.find(std::string(InstallID));
  if (Found == State->PackageInstalls.end())
    throw Error("unknown_package_install");
  const auto &A = Found->second;
  const auto Total = Kind == "scripts"    ? A.Scripts.size()
                     : Kind == "sources"  ? A.Sources.size()
                     : Kind == "requests" ? A.Requests.size()
                     : Kind == "associations"
                         ? A.Associations.size()
                         : throw Error("invalid_record_kind");
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > Total)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Total, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    if (Kind == "scripts")
      Items.emplace_back(package_install::record(A.Scripts[I]));
    else if (Kind == "sources")
      Items.emplace_back(package_install::record(A.Sources[I]));
    else if (Kind == "requests")
      Items.emplace_back(package_install::record(A.Requests[I]));
    else
      Items.emplace_back(package_install::record(A.Associations[I]));
  }
  auto Reply = package_install::summary(A, State->Revision);
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

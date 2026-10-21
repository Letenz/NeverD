//===- SessionPackageAdvisories.cpp - Captured advisory selections --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Revision-bound imports, shared metadata ceilings and atomic version
/// matching.
///
//===----------------------------------------------------------------------===//

#include "JsonReader.h"
#include "SessionInternal.h"
#include "packages/advisories/Publication.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
namespace {
std::vector<std::string> advisorySelection(std::string_view Options) {
  const auto V = parseBoundedJSON(Options, {4096, 4, 64, 64});
  const auto *O = V.getAsObject();
  if (!O || O->getInteger("schema_version") != 1 || O->size() > 2)
    throw Error("invalid_package_advisory_options");
  for (const auto &[Key, Value] : *O)
    if (Key != "schema_version" && Key != "advisory_ids")
      throw Error("invalid_package_advisory_options");
  std::vector<std::string> IDs;
  if (const auto *A = O->get("advisory_ids")) {
    const auto *List = A->getAsArray();
    if (!List || List->size() > MaxPackageAdvisoryImports)
      throw Error("invalid_package_advisory_options");
    for (const auto &V : *List) {
      const auto ID = V.getAsString();
      if (!ID || ID->size() != 64 ||
          ID->find_first_not_of("0123456789abcdef") != ID->npos)
        throw Error("invalid_package_advisory_options");
      IDs.push_back(ID->str());
    }
  }
  std::sort(IDs.begin(), IDs.end());
  if (std::adjacent_find(IDs.begin(), IDs.end()) != IDs.end())
    throw Error("package_advisory_duplicate_selection");
  return IDs;
}
template <class Row>
llvm::json::Object page(llvm::json::Object Summary,
                        const std::vector<Row> &Rows, uint64_t Offset,
                        uint64_t Limit) {
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > Rows.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Rows.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(package_advisories::record(Rows[I]));
  Summary["items"] = std::move(Items);
  Summary["offset"] = Offset;
  Summary["next_offset"] =
      End == Rows.size() ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  Summary["page_complete"] = End == Rows.size();
  return Summary;
}
} // namespace

std::string Session::importPackageAdvisory(std::string_view Revision,
                                           std::string_view ArtifactID,
                                           std::string_view DescriptorID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, A] : State->PackageAdvisories)
    if (A.ArtifactID == ArtifactID && A.Descriptor.ArtifactID == DescriptorID)
      return json(package_advisories::summary(A, State->Revision));
  if (State->PackageAdvisories.size() >= MaxPackageAdvisoryImports)
    throw Error("package_advisory_import_cache_budget_exceeded");
  auto Select = [&](std::string_view ID) {
    const auto View = State->artifactView(ID);
    if (!View)
      throw Error("unknown_package_metadata_artifact");
    Artifact A;
    A.ID = ID;
    A.BlobHash = View->BlobHash;
    A.Content = View->Content;
    return A;
  };
  const PackageEvidenceDescriptor *Existing = nullptr;
  for (const auto &[ID, D] : State->PackageEvidenceDescriptors)
    if (D.ArtifactID == DescriptorID)
      Existing = &D;
  if (!Existing &&
      State->PackageEvidenceDescriptors.size() >= MaxPackageEvidenceDescriptors)
    throw Error("package_evidence_descriptor_cache_budget_exceeded");
  const auto D = Existing ? *Existing
                          : readPackageEvidenceDescriptor(Select(DescriptorID));
  auto A = readPackageAdvisory(Select(ArtifactID), D);
  const auto Bytes = A.RetainedBytes + (Existing ? 0 : D.RetainedBytes);
  if (Bytes > MaxPackageEvidenceRetainedBytes - State->PackageEvidenceBytes)
    throw Error("package_evidence_retained_budget_exceeded");
  auto Reply = json(package_advisories::summary(A, State->Revision));
  const auto ID = A.ID;
  bool AddedDescriptor = false;
  try {
    if (!Existing) {
      AddedDescriptor =
          State->PackageEvidenceDescriptors.emplace(D.ID, D).second;
      if (!AddedDescriptor)
        throw Error("package_evidence_identity_conflict");
    }
    if (!State->PackageAdvisories.emplace(ID, std::move(A)).second)
      throw Error("package_advisory_identity_conflict");
  } catch (...) {
    if (AddedDescriptor)
      State->PackageEvidenceDescriptors.erase(D.ID);
    throw;
  }
  State->PackageEvidenceBytes += Bytes;
  return Reply;
}

std::string Session::packageAdvisoryRecords(std::string_view Revision,
                                            std::string_view ID,
                                            std::string_view Kind,
                                            uint64_t Offset,
                                            uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageAdvisories.find(std::string(ID));
  if (Found == State->PackageAdvisories.end())
    throw Error("unknown_package_advisory");
  const auto &A = Found->second;
  auto Summary = package_advisories::summary(A, State->Revision);
  Summary["record_kind"] = std::string(Kind);
  if (Kind == "affected")
    return json(page(std::move(Summary), A.Affected, Offset, Limit));
  if (Kind == "severities")
    return json(page(std::move(Summary), A.Severities, Offset, Limit));
  throw Error("invalid_record_kind");
}

std::string Session::matchPackageAdvisories(std::string_view Revision,
                                            std::string_view PackageID,
                                            std::string_view Options) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto IDs = advisorySelection(Options);
  const auto P = State->PackageAnalyses.find(std::string(PackageID));
  if (P == State->PackageAnalyses.end())
    throw Error("unknown_package_analysis");
  std::vector<const PackageAdvisory *> Inputs;
  for (const auto &ID : IDs) {
    const auto A = State->PackageAdvisories.find(ID);
    if (A == State->PackageAdvisories.end())
      throw Error("unknown_package_advisory");
    Inputs.push_back(&A->second);
  }
  for (const auto &[ID, M] : State->PackageAdvisoryMatchResults)
    if (M.PackageAnalysisID == PackageID && M.AdvisoryIDs == IDs)
      return json(package_advisories::summary(M, State->Revision));
  if (State->PackageAdvisoryMatchResults.size() >=
      MaxCachedPackageAdvisoryMatches)
    throw Error("package_advisory_match_cache_budget_exceeded");
  auto M = web::matchPackageAdvisories(P->second, Inputs);
  auto Reply = json(package_advisories::summary(M, State->Revision));
  const auto ID = M.ID;
  State->PackageAdvisoryMatchResults.emplace(ID, std::move(M));
  return Reply;
}

std::string Session::packageAdvisoryMatchRecords(std::string_view Revision,
                                                 std::string_view ID,
                                                 uint64_t Offset,
                                                 uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageAdvisoryMatchResults.find(std::string(ID));
  if (Found == State->PackageAdvisoryMatchResults.end())
    throw Error("unknown_package_advisory_matches");
  const auto &M = Found->second;
  return json(page(package_advisories::summary(M, State->Revision), M.Records,
                   Offset, Limit));
}
} // namespace neverd::web

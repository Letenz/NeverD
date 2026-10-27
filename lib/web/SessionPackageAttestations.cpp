//===- SessionPackageAttestations.cpp - Bound attestation selections -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic captured metadata imports and explicit original-byte comparisons.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"
#include "packages/attestations/Publication.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
namespace {
template <class Row>
std::string page(llvm::json::Object Summary, const std::vector<Row> &Rows,
                 uint64_t Offset, uint64_t Limit) {
  if (!Limit || Limit > ::neverd::limits::web::kMaxSessionPageRows ||
      Offset > Rows.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Rows.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(package_attestations::record(Rows[I]));
  Summary["items"] = std::move(Items);
  Summary["offset"] = Offset;
  Summary["next_offset"] =
      End == Rows.size() ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  Summary["page_complete"] = End == Rows.size();
  return json(std::move(Summary));
}
} // namespace

std::string Session::importPackageAttestation(std::string_view Revision,
                                              std::string_view ArtifactID,
                                              std::string_view DescriptorID,
                                              std::string_view Kind) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  for (const auto &[ID, A] : State->PackageAttestations)
    if (A.ArtifactID == ArtifactID && A.Descriptor.ArtifactID == DescriptorID &&
        A.Kind == Kind)
      return json(package_attestations::summary(A, State->Revision));
  if (State->PackageAttestations.size() >= MaxPackageAttestationImports)
    throw Error("package_attestation_import_cache_budget_exceeded");
  const auto Select = [&](std::string_view ID) {
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
  auto A = readPackageAttestation(Select(ArtifactID), D, Kind);
  const auto Bytes = A.RetainedBytes + (Existing ? 0 : D.RetainedBytes);
  if (Bytes > MaxPackageEvidenceRetainedBytes - State->PackageEvidenceBytes)
    throw Error("package_evidence_retained_budget_exceeded");
  auto Reply = json(package_attestations::summary(A, State->Revision));
  const auto ID = A.ID;
  bool AddedDescriptor = false;
  try {
    if (!Existing) {
      AddedDescriptor =
          State->PackageEvidenceDescriptors.emplace(D.ID, D).second;
      if (!AddedDescriptor)
        throw Error("package_evidence_identity_conflict");
    }
    if (!State->PackageAttestations.emplace(ID, std::move(A)).second)
      throw Error("package_attestation_identity_conflict");
  } catch (...) {
    if (AddedDescriptor)
      State->PackageEvidenceDescriptors.erase(D.ID);
    throw;
  }
  State->PackageEvidenceBytes += Bytes;
  return Reply;
}

std::string Session::packageAttestationRecords(std::string_view Revision,
                                               std::string_view ID,
                                               std::string_view Kind,
                                               uint64_t Offset,
                                               uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageAttestations.find(std::string(ID));
  if (Found == State->PackageAttestations.end())
    throw Error("unknown_package_attestation");
  const auto &A = Found->second;
  auto Summary = package_attestations::summary(A, State->Revision);
  Summary["record_kind"] = std::string(Kind);
  if (Kind == "signatures")
    return page(std::move(Summary), A.Signatures, Offset, Limit);
  if (Kind == "subjects")
    return page(std::move(Summary), A.Subjects, Offset, Limit);
  if (Kind == "digests")
    return page(std::move(Summary), A.Digests, Offset, Limit);
  throw Error("invalid_record_kind");
}

std::string Session::matchPackageAttestationDigest(std::string_view Revision,
                                                   std::string_view ArtifactID,
                                                   std::string_view ID,
                                                   std::string_view DigestID) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageAttestations.find(std::string(ID));
  if (Found == State->PackageAttestations.end())
    throw Error("unknown_package_attestation");
  const auto Original = std::find_if(
      State->Published.Artifacts.begin(), State->Published.Artifacts.end(),
      [&](const auto &A) { return A.ID == ArtifactID; });
  if (Original == State->Published.Artifacts.end())
    throw Error("integrity_original_not_captured");
  for (const auto &[Key, M] : State->PackageAttestationDigests)
    if (M.AttestationID == ID && M.DigestID == DigestID &&
        M.Integrity.ArtifactID == ArtifactID)
      return json(package_attestations::summary(M, State->Revision));
  if (State->PackageAttestationDigests.size() >=
      MaxPackageAttestationDigestMatches)
    throw Error("package_attestation_digest_cache_budget_exceeded");
  auto M =
      web::matchPackageAttestationDigest(*Original, Found->second, DigestID);
  auto Reply = json(package_attestations::summary(M, State->Revision));
  const auto Key = M.ID;
  State->PackageAttestationDigests.emplace(Key, std::move(M));
  return Reply;
}

std::string Session::packageAttestationDigestResult(std::string_view Revision,
                                                    std::string_view ID) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->PackageAttestationDigests.find(std::string(ID));
  if (Found == State->PackageAttestationDigests.end())
    throw Error("unknown_package_attestation_digest_match");
  return json(package_attestations::summary(Found->second, State->Revision));
}
} // namespace neverd::web

//===- Integrity.cpp - Original, declaration and namespace relationships -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Digest equality, declaration identity and inventory ancestry stay distinct.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include <algorithm>

namespace neverd::web::package_assessment {
using package_evidence::requireIdentity;

void Composer::integrityAnchor(const PackageIntegrityResult &R) {
  for (const auto &ID :
       {R.ID, R.ArtifactID, R.BlobHash, R.DeclarationArtifactID,
        R.DeclarationHash, R.SelectionID})
    requireIdentity(ID);
  charge(1024);
  const auto O = Originals.find(R.ArtifactID);
  if (O == Originals.end() || O->second->Directory ||
      O->second->BlobHash != R.BlobHash ||
      R.ID != identity("package-integrity",
                       {R.ArtifactID, R.BlobHash, R.DeclarationArtifactID,
                        R.DeclarationHash, R.SelectionID, R.Kind,
                        PackageIntegrityProfile}))
    throw Error("package_assessment_original_mismatch");
  // Status is owned by the SRI reader. Refuse foreign model text rather than
  // publishing private target strings as an evidence category.
  constexpr std::string_view States[] = {
      "match",
      "mismatch",
      "missing_declaration",
      "invalid",
      "unsupported_algorithm",
      "unsupported_sri_options",
      "git_commit_declared_unverified",
      "integrity_declaration_budget_exceeded"};
  if (std::find(std::begin(States), std::end(States), R.Status) ==
      std::end(States))
    throw Error("package_assessment_integrity_state_mismatch");
  if (!R.Algorithm.empty() && R.Algorithm != "sha1" &&
      R.Algorithm != "sha256" && R.Algorithm != "sha384" &&
      R.Algorithm != "sha512")
    throw Error("package_assessment_integrity_state_mismatch");
}

bool Composer::originalContainsManifest(const PackageIntegrityResult &R,
                                        const PackageInstance &I) {
  if (!Archive || Archive->ArtifactID != R.ArtifactID ||
      Archive->BlobHash != R.BlobHash || I.ManifestArtifactID.empty())
    return false;
  charge(1024);
  const auto M = ArchiveMembers.find(I.ManifestArtifactID);
  const auto F = Members.find(I.ManifestArtifactID);
  if (M == ArchiveMembers.end() || F == Members.end() ||
      !M->second->available() || M->second->BlobHash != F->second->BlobHash ||
      F->second->ParentID != Archive->ID)
    throw Error("package_assessment_namespace_mismatch");
  // Only a manifest belonging to the selected root has this relationship.
  // A container may include dependencies; no dependency-tree proof follows.
  return Root && I.ID == Root->ID;
}

void Composer::integrity() {
  coverage("original_integrity",
           Integrities.empty() ? "" : Integrities.begin()->first);
  for (const auto &[ID, R] : Integrities) {
    integrityAnchor(*R);
    PackageFinding F;
    F.Kind = PackageFindingKind::Integrity;
    F.OwnerID = F.RecordID = R->ID;
    F.Status = R->Status;
    F.DigestAlgorithm = R->Algorithm;
    F.ArtifactID = R->ArtifactID;
    F.BlobHash = R->BlobHash;
    F.DeclarationArtifactID = R->DeclarationArtifactID;
    F.DeclarationHash = R->DeclarationHash;
    F.Confidence = "selected_original_digest";
    F.Binding = "original_inventory_unbound";
    if (R->Kind == "lock_package") {
      const auto &I = instance(R->SelectionID);
      if (P.Kind != "npm-lock" || R->DeclarationArtifactID != P.ArtifactID ||
          R->DeclarationHash != P.BlobHash)
        throw Error("package_assessment_declaration_mismatch");
      F.PackageID = I.ID;
      F.RelatedIDs.push_back(P.ID);
      // The selected lock declaration is bound, but a dependency archive does
      // not acquire installed-tree ancestry merely through that selection.
      F.Binding = "lock_instance_original_inventory_unbound";
      if (originalContainsManifest(*R, I))
        F.Binding = "original_contains_root_manifest_and_lock_declaration";
    } else if (R->Kind == "registry_dist") {
      if (R->SelectionID != R->DeclarationArtifactID)
        throw Error("package_assessment_declaration_mismatch");
      const PackageAttestation *Metadata = nullptr;
      for (const auto &[ID, T] : Attestations) {
        charge(256);
        if (T->Kind == "registry-dist" &&
            T->ArtifactID == R->DeclarationArtifactID &&
            T->BlobHash == R->DeclarationHash) {
          if (Metadata)
            throw Error("package_assessment_declaration_mismatch");
          Metadata = T;
        }
      }
      if (Metadata) {
        F.RelatedIDs.push_back(Metadata->ID);
        if (Metadata->PackageIdentityStatus ==
            "conflicting_registry_declarations")
          throw Error("package_assessment_declaration_mismatch");
        if (Root && Root->Name && !Root->Name->empty() && Root->Version &&
            !Root->Version->empty() &&
            (Root->NameSource == "manifest" ||
             Root->NameSource == "lockfile") &&
            Root->VersionKind == "package_version_declaration" &&
            Root->ManifestStatus != "conflicting_declarations" &&
            P.LegacyStatus != "conflicting_declarations" &&
            Metadata->PackageIdentityStatus == "name_and_version_declared") {
          charge(Root->Name->size() + Root->Version->size() +
                 Metadata->Name.size() + Metadata->Version.size());
          if (*Root->Name != Metadata->Name ||
              *Root->Version != Metadata->Version)
            throw Error("package_assessment_declaration_mismatch");
          F.PackageID = Root->ID;
          F.Binding = originalContainsManifest(*R, *Root)
                          ? "original_contains_declared_root_manifest"
                          : "declared_identity_original_inventory_unbound";
        }
      }
    } else {
      throw Error("package_assessment_integrity_kind_mismatch");
    }
    append(std::move(F));
  }
}
} // namespace neverd::web::package_assessment

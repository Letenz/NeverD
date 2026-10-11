//===- Metadata.cpp - Selected advisory and attestation coverage --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve declaration relationships and admitted feed times without trust.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include <algorithm>

namespace neverd::web::package_assessment {
namespace {
void time(PackageAssessmentFeed &F, std::string_view Kind,
          const PackageEvidenceTime &T) {
  F.Times.push_back({std::string(Kind), T.Status,
                     T.Status == "valid_utc_timestamp" ? T.Value : ""});
}
} // namespace
void Composer::feed(std::string_view OwnerID,
                    const PackageEvidenceDescriptor &D) {
  using package_evidence::requireIdentity;
  requireIdentity(D.ID);
  requireIdentity(D.ArtifactID);
  requireIdentity(D.BlobHash);
  PackageAssessmentFeed F;
  F.OwnerID = OwnerID;
  F.DescriptorID = D.ID;
  F.ArtifactID = D.ArtifactID;
  F.BlobHash = D.BlobHash;
  F.Validation = D.Status;
  time(F, "retrieved_at", D.RetrievedAt);
  time(F, "as_of", D.AsOf);
  charge(512);
  retain(2048);
  A.Feeds.push_back(std::move(F));
}

void Composer::metadata() {
  const auto *Matches = Selection.AdvisoryMatches;
  coverage("advisory_matching", Matches ? Matches->ID : "");
  if (Matches) {
    if (Matches->PackageAnalysisID != P.ID ||
        Matches->AdvisoryIDs.size() != Advisories.size() ||
        Matches->Records.size() > MaxPackageAdvisoryMatches)
      throw Error("package_assessment_advisory_mismatch");
    std::vector<std::string_view> IDs;
    for (const auto &ID : Matches->AdvisoryIDs) {
      if (!Advisories.contains(ID))
        throw Error("package_assessment_advisory_mismatch");
      IDs.push_back(ID);
    }
    std::sort(IDs.begin(), IDs.end());
    charge(IDs.size() * 1024);
    if (std::adjacent_find(IDs.begin(), IDs.end()) != IDs.end())
      throw Error("package_assessment_advisory_mismatch");
    for (const auto &R : Matches->Records) {
      (void)instance(R.PackageID);
      const auto T = Advisories.find(R.AdvisoryID);
      if (T == Advisories.end())
        throw Error("package_assessment_advisory_mismatch");
      constexpr std::string_view States[] = {
          "coverage_unknown", "declared_version_match",
          "version_not_in_admitted_selectors",
          "no_matching_package_declaration"};
      if (std::find(std::begin(States), std::end(States), R.Status) ==
          std::end(States))
        throw Error("package_assessment_advisory_mismatch");
      PackageFinding F;
      F.Kind = PackageFindingKind::Advisory;
      F.OwnerID = Matches->ID;
      F.RecordID = R.ID;
      F.PackageID = R.PackageID;
      F.Status = R.Status;
      F.ArtifactID = T->second->ArtifactID;
      F.BlobHash = T->second->BlobHash;
      F.RelatedIDs.push_back(R.AdvisoryID);
      F.Binding = "declared_package_version_selector";
      F.Confidence = "supplied_advisory_not_exploitability";
      append(std::move(F));
    }
  } else if (!Advisories.empty()) {
    // Imports can be selected without matching; they remain explicit coverage.
    coverage("advisory_imports_unmatched", Advisories.begin()->first);
  }
  for (const auto &[ID, T] : Advisories) {
    feed(ID, T->Descriptor);
    auto &F = A.Feeds.back();
    F.TemporalStatus = T->TemporalStatus;
    time(F, "advisory_modified", T->Modified);
    time(F, "advisory_published", T->Published);
    time(F, "advisory_withdrawn", T->Withdrawn);
    PackageFinding Declaration;
    Declaration.Kind = PackageFindingKind::Advisory;
    Declaration.OwnerID = Declaration.RecordID = ID;
    Declaration.ArtifactID = T->ArtifactID;
    Declaration.BlobHash = T->BlobHash;
    Declaration.Status = "selected_advisory_declaration";
    Declaration.Binding = "metadata_inventory_unbound";
    Declaration.Confidence = "supplied_advisory_not_exploitability";
    append(std::move(Declaration));
  }

  coverage("attestation_metadata",
           Attestations.empty() ? "" : Attestations.begin()->first);
  for (const auto &[ID, T] : Attestations) {
    if (T->Signatures.size() > MaxPackageAttestationSignatures ||
        T->Subjects.size() > MaxPackageAttestationSubjects ||
        T->Digests.size() > MaxPackageAttestationDigests)
      throw Error("package_assessment_attestation_mismatch");
    feed(ID, T->Descriptor);
    A.Feeds.back().TemporalStatus = T->BuildTimeStatus;
    time(A.Feeds.back(), "build_started", T->Started);
    time(A.Feeds.back(), "build_finished", T->Finished);
    PackageFinding F;
    F.Kind = PackageFindingKind::Attestation;
    F.OwnerID = F.RecordID = ID;
    F.ArtifactID = T->ArtifactID;
    F.BlobHash = T->BlobHash;
    F.Confidence = "supplied_metadata_unauthenticated";
    F.Binding = "metadata_inventory_unbound";
    if (T->Kind == "registry-dist") {
      F.Status = T->PackageIdentityStatus == "conflicting_registry_declarations"
                     ? "registry_identity_conflicted"
                     : "registry_metadata_unverified";
    } else if (T->Kind == "intoto-statement" || T->Kind == "dsse-intoto") {
      F.Status = T->StatementStatus == "admitted_statement_v1_subset"
                     ? "statement_subset_unverified"
                     : "statement_coverage_unknown";
    } else {
      throw Error("package_assessment_attestation_mismatch");
    }
    append(F);
    for (const auto &S : T->Signatures) {
      F.RecordID = S.ID;
      F.Status = "signature_declaration_unverified";
      append(F);
    }
    for (const auto &S : T->Subjects) {
      F.RecordID = S.ID;
      F.Status = "subject_declaration_unverified";
      append(F);
    }
  }

  coverage("attestation_original_digest",
           DigestMatches.empty() ? "" : DigestMatches.begin()->first);
  for (const auto &[ID, M] : DigestMatches) {
    const auto T = Attestations.find(M->AttestationID);
    if (T == Attestations.end() ||
        M->Integrity.DeclarationArtifactID != T->second->ArtifactID ||
        M->Integrity.DeclarationHash != T->second->BlobHash ||
        M->Integrity.SelectionID != M->DigestID ||
        M->Integrity.Kind != "intoto_subject_digest")
      throw Error("package_assessment_attestation_mismatch");
    const auto &Digests = T->second->Digests;
    charge(Digests.size() * 256);
    if (std::none_of(Digests.begin(), Digests.end(), [&](const auto &D) {
          return D.ID == M->DigestID && D.SubjectID == M->SubjectID;
        }))
      throw Error("package_assessment_attestation_mismatch");
    integrityAnchor(M->Integrity);
    PackageFinding F;
    F.Kind = PackageFindingKind::Attestation;
    F.OwnerID = M->ID;
    F.RecordID = M->DigestID;
    F.ArtifactID = M->Integrity.ArtifactID;
    F.BlobHash = M->Integrity.BlobHash;
    F.DeclarationArtifactID = M->Integrity.DeclarationArtifactID;
    F.DeclarationHash = M->Integrity.DeclarationHash;
    F.RelatedIDs = {M->AttestationID, M->SubjectID, M->Integrity.ID};
    F.Status = M->Integrity.Status;
    F.DigestAlgorithm = M->Integrity.Algorithm;
    F.Binding = "selected_subject_original_only";
    F.Confidence = "digest_comparison_unauthenticated";
    append(std::move(F));
  }
}
} // namespace neverd::web::package_assessment

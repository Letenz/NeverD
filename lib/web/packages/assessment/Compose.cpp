//===- Compose.cpp - Bounded retained package evidence composition ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One immutable selection identity and independent coverage for every owner.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
std::string_view packageFindingKindName(PackageFindingKind Kind) {
  switch (Kind) {
  case PackageFindingKind::Coverage:
    return "coverage";
  case PackageFindingKind::Inventory:
    return "inventory";
  case PackageFindingKind::Integrity:
    return "integrity";
  case PackageFindingKind::Advisory:
    return "advisory";
  case PackageFindingKind::Attestation:
    return "attestation";
  case PackageFindingKind::Behavior:
    return "behavior";
  case PackageFindingKind::Installation:
    return "installation";
  case PackageFindingKind::Comparison:
    return "comparison";
  }
  throw Error("invalid_package_finding_kind");
}
PackageAssessment
composePackageAssessment(const PackageAnalysis &P, const Snapshot &Namespace,
                         const PackageAssessmentSelection &Selection) {
  return package_assessment::Composer(P, Namespace, Selection).run();
}
} // namespace neverd::web

namespace neverd::web::package_assessment {
using package_evidence::requireIdentity;

void Composer::charge(uint64_t Amount) {
  if (Amount > MaxPackageAssessmentSteps - A.Steps)
    throw Error("package_assessment_work_budget_exceeded");
  A.Steps += Amount;
}
void Composer::retain(uint64_t Amount) {
  if (Amount > MaxPackageAssessmentBytes - A.RetainedBytes)
    throw Error("package_assessment_retained_budget_exceeded");
  A.RetainedBytes += Amount;
}

Composer::Composer(const PackageAnalysis &P, const Snapshot &Namespace,
                   const PackageAssessmentSelection &Selection)
    : P(P), Namespace(Namespace), Selection(Selection) {
  requireIdentity(Namespace.ID);
  requireIdentity(P.ID);
  requireIdentity(P.ArtifactID);
  requireIdentity(P.BlobHash);
  if (P.ID !=
      identity("package-analysis", {Namespace.ID, P.ArtifactID, P.BlobHash,
                                    P.Kind, PackageProfile}))
    throw Error("package_assessment_namespace_mismatch");
  if (P.Packages.empty() || P.Packages.size() > MaxPackageInstances ||
      Namespace.Artifacts.size() > Limits::HardEntries + 1 ||
      Selection.Archives.size() > 4 || Selection.Integrity.size() > 16 ||
      Selection.Advisories.size() > MaxPackageAdvisoryImports ||
      Selection.Attestations.size() > MaxPackageAttestationImports ||
      Selection.DigestMatches.size() > MaxPackageAttestationDigestMatches)
    throw Error("package_assessment_selection_budget_exceeded");
  A.PackageAnalysisID = P.ID;
  A.NamespaceID = Namespace.ID;
  for (const auto &F : Namespace.Artifacts) {
    charge(512); // Bounded 64-byte ordered-index comparisons and insertion.
    requireIdentity(F.ID);
    if (!Members.emplace(F.ID, &F).second)
      throw Error("package_assessment_namespace_mismatch");
  }
  const auto Selected = Members.find(P.ArtifactID);
  if (Selected == Members.end() || Selected->second->BlobHash != P.BlobHash)
    throw Error("package_assessment_namespace_mismatch");
  for (const auto &I : P.Packages) {
    charge(512);
    requireIdentity(I.ID);
    if (!Instances.emplace(I.ID, &I).second)
      throw Error("package_assessment_package_mismatch");
    if (I.Location.empty()) {
      if (Root)
        throw Error("package_assessment_package_mismatch");
      Root = &I;
    }
  }
  if (Selection.Originals) {
    requireIdentity(Selection.Originals->ID);
    if (Selection.Originals->Artifacts.size() > Limits::HardEntries + 1)
      throw Error("package_assessment_selection_budget_exceeded");
    for (const auto &F : Selection.Originals->Artifacts) {
      charge(512);
      requireIdentity(F.ID);
      if (!Originals.emplace(F.ID, &F).second)
        throw Error("package_assessment_original_mismatch");
    }
  }
  for (const auto *R : Selection.Archives) {
    if (!R)
      throw Error("package_assessment_selection_mismatch");
    requireIdentity(R->ID);
    if (R->ID == Namespace.ID) {
      if (Archive || R->Members.size() > MaxPackageArchiveMembers)
        throw Error("package_assessment_namespace_mismatch");
      Archive = R;
      for (const auto &M : R->Members) {
        charge(512);
        requireIdentity(M.ID);
        if (M.ParentID != Archive->ID ||
            !ArchiveMembers.emplace(M.ID, &M).second)
          throw Error("package_assessment_namespace_mismatch");
      }
    }
  }
  if (Archive) {
    const auto M = ArchiveMembers.find(P.ArtifactID);
    if (M == ArchiveMembers.end() || !M->second->available() ||
        M->second->BlobHash != P.BlobHash ||
        Selected->second->ParentID != Archive->ID)
      throw Error("package_assessment_namespace_mismatch");
  }
  const auto Add = [&](const auto *R) {
    if (!R)
      throw Error("package_assessment_selection_mismatch");
    requireIdentity(R->ID);
    charge(128);
    A.SelectionIDs.push_back(R->ID);
  };
  for (const auto *R : Selection.Integrity) {
    Add(R);
    Integrities.emplace(R->ID, R);
  }
  for (const auto *R : Selection.Advisories) {
    Add(R);
    if (!Advisories.emplace(R->ID, R).second)
      throw Error("package_assessment_duplicate_selection");
  }
  for (const auto *R : Selection.Attestations) {
    Add(R);
    if (!Attestations.emplace(R->ID, R).second)
      throw Error("package_assessment_duplicate_selection");
  }
  for (const auto *R : Selection.DigestMatches) {
    Add(R);
    DigestMatches.emplace(R->ID, R);
  }
  if (Selection.AdvisoryMatches)
    Add(Selection.AdvisoryMatches);
  if (Selection.Behavior)
    Add(Selection.Behavior);
  if (Selection.Installation)
    Add(Selection.Installation);
  if (Selection.Comparison)
    Add(Selection.Comparison);
  if (Selection.BehaviorComparison)
    Add(Selection.BehaviorComparison);
  std::sort(A.SelectionIDs.begin(), A.SelectionIDs.end());
  charge(A.SelectionIDs.size() * 1024);
  if (std::adjacent_find(A.SelectionIDs.begin(), A.SelectionIDs.end()) !=
      A.SelectionIDs.end())
    throw Error("package_assessment_duplicate_selection");
  std::vector<std::string_view> Key{
      PackageAssessmentProfile, P.ID, Namespace.ID,
      Archive ? std::string_view(Archive->ID) : std::string_view{},
      Selection.Originals ? std::string_view(Selection.Originals->ID)
                          : std::string_view{}};
  for (const auto &ID : A.SelectionIDs)
    Key.push_back(ID);
  A.ID = identity("package-assessment", Key);
  retain(2048 + A.SelectionIDs.size() * 96);
}

void Composer::append(PackageFinding F) {
  if (A.Findings.size() >= MaxPackageAssessmentFindings)
    throw Error("package_assessment_finding_budget_exceeded");
  for (const auto *ID :
       {&F.OwnerID, &F.RecordID, &F.PackageID, &F.ArtifactID, &F.BlobHash,
        &F.SourceID, &F.NodeID, &F.DeclarationArtifactID, &F.DeclarationHash})
    if (!ID->empty())
      requireIdentity(*ID);
  if (F.RelatedIDs.size() >
          ::neverd::limits::web::kComposerAppendFRelatedIDsSizeLimit ||
      (F.HasSourceSpan && F.Start > F.End))
    throw Error("package_assessment_record_mismatch");
  for (const auto &ID : F.RelatedIDs)
    requireIdentity(ID);
  charge(512 + F.RelatedIDs.size() * 128);
  retain(2048 + F.RelatedIDs.size() * 96);
  F.ID = identity("package-assessment-finding",
                  {A.ID, std::to_string(A.Findings.size()),
                   packageFindingKindName(F.Kind), F.OwnerID, F.RecordID,
                   F.Status, F.Binding});
  A.Findings.push_back(std::move(F));
}

void Composer::coverage(std::string_view Surface, std::string_view OwnerID) {
  PackageFinding F;
  F.Kind = PackageFindingKind::Coverage;
  F.Status = OwnerID.empty() ? "not_selected" : "selected_partial_evidence";
  F.Binding = Surface;
  F.OwnerID = OwnerID;
  F.Confidence = "selection_only";
  append(std::move(F));
}
const PackageInstance &Composer::instance(std::string_view ID) {
  charge(512);
  const auto I = Instances.find(ID);
  if (I == Instances.end())
    throw Error("package_assessment_package_mismatch");
  return *I->second;
}
void Composer::inventory() {
  coverage("inventory", P.ID);
  for (const auto &I : P.Packages) {
    PackageFinding F;
    F.Kind = PackageFindingKind::Inventory;
    F.OwnerID = P.ID;
    F.RecordID = F.PackageID = I.ID;
    F.ArtifactID = I.ManifestArtifactID;
    if (!F.ArtifactID.empty()) {
      const auto M = Members.find(F.ArtifactID);
      if (M == Members.end())
        throw Error("package_assessment_namespace_mismatch");
      F.BlobHash = M->second->BlobHash;
    }
    F.Status =
        I.ManifestStatus == "conflicting_declarations" ||
                P.LegacyStatus == "conflicting_declarations"
            ? "identity_conflicted"
        : !I.Name || I.Name->empty() || !I.Version || I.Version->empty() ||
                (I.NameSource != "manifest" && I.NameSource != "lockfile") ||
                I.VersionKind != "package_version_declaration"
            ? "identity_incomplete"
            : "identity_declared";
    F.Binding = "retained_inventory_instance";
    F.Confidence = "declaration_only";
    append(std::move(F));
  }
}
PackageAssessment Composer::run() {
  inventory();
  integrity();
  metadata();
  behavior();
  return std::move(A);
}
} // namespace neverd::web::package_assessment

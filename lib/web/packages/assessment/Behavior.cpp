//===- Behavior.cpp - Package source, installation and comparison joins -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Consume exact owner selections; source syntax never becomes runtime proof.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

namespace neverd::web::package_assessment {
void Composer::behavior() {
  const auto *B = Selection.Behavior;
  coverage("source_behavior", B ? B->ID : "");
  if (B) {
    if (B->PackageAnalysisID != P.ID ||
        B->Sources.size() > MaxPackageBehaviorSources)
      throw Error("package_assessment_behavior_mismatch");
    for (const auto &S : B->Sources) {
      if (!S.PackageID.empty())
        (void)instance(S.PackageID);
      if (S.Records.size() > MaxPackageBehaviorRecords ||
          (S.Status != "partial" && S.Status != "unavailable" &&
           S.Status != "budget_exceeded") ||
          (S.Status != "partial" && !S.Records.empty()))
        throw Error("package_assessment_behavior_mismatch");
      PackageFinding F;
      F.Kind = PackageFindingKind::Behavior;
      F.OwnerID = B->ID;
      F.RecordID = F.SourceID = S.SourceID;
      F.PackageID = S.PackageID;
      F.ArtifactID = S.ArtifactID;
      F.BlobHash = S.SourceHash;
      F.Status = S.Status == "partial" ? "selected_syntax_partial"
                                       : "selected_syntax_unavailable";
      F.Binding = "selected_source_delivery_candidate";
      F.Confidence = "static_syntax_not_runtime";
      append(F);
      for (const auto &R : S.Records) {
        F.RecordID = R.ID;
        F.NodeID = R.NodeID;
        F.Status = "recognized_source_syntax";
        F.API = R.API;
        F.PatternKind = R.Kind;
        F.OriginEvidence = R.OriginEvidence;
        F.Start = R.Start;
        F.End = R.End;
        F.HasSourceSpan = true;
        append(F);
      }
    }
  }
  const auto *I = Selection.Installation;
  coverage("installation_traces", I ? I->ID : "");
  if (I) {
    if (I->PackageAnalysisID != P.ID || I->NamespaceID != Namespace.ID ||
        (B && !I->PackageBehaviorID.empty() && I->PackageBehaviorID != B->ID) ||
        I->Scripts.size() > MaxPackageInstallScripts ||
        I->Requests.size() > MaxPackageInstallRequests ||
        I->Associations.size() > MaxPackageInstallAssociations)
      throw Error("package_assessment_installation_mismatch");
    for (const auto &R : I->Scripts) {
      if (!R.PackageID.empty())
        (void)instance(R.PackageID);
      PackageFinding F;
      F.Kind = PackageFindingKind::Installation;
      F.OwnerID = I->ID;
      F.RecordID = R.ID;
      F.PackageID = R.PackageID;
      F.ArtifactID = R.ManifestID;
      F.Status = "lifecycle_declaration_not_activation";
      F.Binding = "retained_script_trace";
      F.Confidence = "static_candidate_only";
      append(std::move(F));
    }
    for (const auto &R : I->Requests) {
      PackageFinding F;
      F.Kind = PackageFindingKind::Installation;
      F.OwnerID = I->ID;
      F.RecordID = R.ID;
      F.SourceID = R.SourceID;
      F.NodeID = R.NodeID;
      F.Status = R.TargetSourceID.empty() ? "module_request_unresolved"
                                          : "selected_module_file_candidate";
      F.Binding = "retained_module_request";
      F.Confidence = "static_candidate_only";
      append(std::move(F));
    }
    for (const auto &R : I->Associations) {
      if (!R.PackageID.empty())
        (void)instance(R.PackageID);
      PackageFinding F;
      F.Kind = PackageFindingKind::Installation;
      F.OwnerID = I->ID;
      F.RecordID = R.ID;
      F.PackageID = R.PackageID;
      F.SourceID = R.SourceID;
      F.NodeID = R.NodeID;
      F.RelatedIDs = {R.ScriptTraceID, R.RecordID, R.BehaviorID};
      F.Status = "lifecycle_source_syntax_association";
      F.API = R.API;
      F.PatternKind = R.Kind;
      F.OriginEvidence = R.OriginEvidence;
      F.Binding = "selected_installation_candidate_chain";
      F.Confidence = "static_syntax_not_execution";
      F.Start = R.Start;
      F.End = R.End;
      F.HasSourceSpan = true;
      append(std::move(F));
    }
  }
  const auto *D = Selection.Comparison;
  coverage("inventory_comparison", D ? D->ID : "");
  if (D) {
    if ((D->BeforeID != P.ID && D->AfterID != P.ID) ||
        D->Changes.size() > MaxPackageRecords)
      throw Error("package_assessment_comparison_mismatch");
    for (const auto &R : D->Changes) {
      PackageFinding F;
      F.Kind = PackageFindingKind::Comparison;
      F.OwnerID = D->ID;
      F.RecordID = R.ID;
      F.RelatedIDs = {D->BeforeID, D->AfterID};
      F.Status = "declared_inventory_change";
      F.Binding = D->BeforeID == P.ID ? "selected_before_inventory"
                                      : "selected_after_inventory";
      F.Confidence = "declaration_comparison_only";
      append(std::move(F));
    }
  }
  const auto *BD = Selection.BehaviorComparison;
  coverage("selected_behavior_comparison", BD ? BD->ID : "");
  if (BD) {
    if (!D || BD->PackageDiffID != D->ID || BD->BeforeID != D->BeforeID ||
        BD->AfterID != D->AfterID ||
        (B && (BD->BeforeID == P.ID ? BD->BeforeBehaviorID
                                    : BD->AfterBehaviorID) != B->ID) ||
        BD->Pairs.size() > MaxPackageBehaviorDiffPairs ||
        BD->Occurrences.size() > MaxPackageBehaviorDiffOccurrences ||
        BD->Patterns.size() > MaxPackageBehaviorDiffOccurrences)
      throw Error("package_assessment_comparison_mismatch");
    for (const auto &R : BD->Pairs) {
      PackageFinding F;
      F.Kind = PackageFindingKind::Comparison;
      F.OwnerID = BD->ID;
      F.RecordID = R.ID;
      F.Status = R.Status == "compared_recognized_patterns"
                     ? "selected_syntax_counts_compared"
                     : "selected_comparison_coverage_unavailable";
      F.Binding = "selected_source_pair_candidates";
      F.Confidence = "syntax_counts_not_call_correspondence";
      append(std::move(F));
    }
    for (const auto &R : BD->Patterns) {
      PackageFinding F;
      F.Kind = PackageFindingKind::Comparison;
      F.OwnerID = BD->ID;
      F.RecordID = R.ID;
      F.RelatedIDs = {R.PairID};
      F.Status = "selected_recognized_pattern_counts";
      F.Binding = "selected_source_pair_candidates";
      F.Confidence = "syntax_counts_not_call_correspondence";
      F.API = R.API;
      F.PatternKind = R.Kind;
      F.OriginEvidence = R.OriginEvidence;
      F.BeforeCount = R.Before;
      F.AfterCount = R.After;
      F.HasPatternCounts = true;
      append(std::move(F));
    }
    for (const auto &R : BD->Occurrences) {
      PackageFinding F;
      F.Kind = PackageFindingKind::Comparison;
      F.OwnerID = BD->ID;
      F.RecordID = R.ID;
      F.SourceID = R.SourceID;
      F.ArtifactID = R.ArtifactID;
      F.NodeID = R.NodeID;
      F.Start = R.Start;
      F.End = R.End;
      F.HasSourceSpan = true;
      F.RelatedIDs = {R.RecordID, R.GroupID, R.PairID};
      F.Status = "selected_comparison_source_occurrence";
      F.Binding = "retained_source_pair_occurrence";
      F.Confidence = "static_syntax_not_runtime";
      append(std::move(F));
    }
  }
}
} // namespace neverd::web::package_assessment

//===- Report.cpp - Readable evidence-preserving package review ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Markdown pages contain only fixed prose, admitted times and anchors.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web::package_assessment {
std::string markdown(const PackageAssessment &A,
                     const PackageReviewState &Review, uint64_t Offset,
                     uint64_t Limit) {
  if (!Limit || Limit > ::neverd::limits::web::kReportMarkdownLimit ||
      Offset > A.Findings.size())
    throw Error("invalid_page");
  const auto Effective = dispositions(A, Review);
  const auto End = std::min<uint64_t>(A.Findings.size(), Offset + Limit);
  std::string Text = "# Offline package evidence review\n\n";
  const auto Anchor = [&](std::string_view Label, std::string_view ID) {
    if (!ID.empty())
      Text += std::string(Label) + ": `" + std::string(ID) + "`\n\n";
  };
  Anchor("Assessment", A.ID);
  Anchor("Package analysis", A.PackageAnalysisID);
  Anchor("Namespace", A.NamespaceID);
  Anchor("Review head", Review.HeadID);
  Text +=
      "Coverage is partial and limited to explicitly selected evidence. "
      "Unselected or unflagged content has no safe-package conclusion. "
      "Digest equality concerns selected original bytes; it does not verify "
      "an installed dependency tree. Signatures and publisher identity are "
      "unauthenticated. Runtime behavior, reachability and feed freshness "
      "have not been verified. Dispositions are caller assertions.\n\n";
  Text += "Findings " + std::to_string(Offset) + ".." + std::to_string(End) +
          " of " + std::to_string(A.Findings.size()) + ".\n\n";
  for (const auto &F : A.Feeds) {
    Anchor("Feed evidence", F.OwnerID);
    Anchor("Descriptor", F.DescriptorID);
    Anchor("Descriptor artifact", F.ArtifactID);
    Anchor("Descriptor SHA-256", F.BlobHash);
    Text += "Descriptor validation: `" + F.Validation + "`.\n\n";
    if (!F.TemporalStatus.empty())
      Text += "Declared chronology: `" + F.TemporalStatus + "`.\n\n";
    for (const auto &T : F.Times) {
      Text += T.Kind + ": `" + T.Status + "`";
      if (!T.Value.empty())
        Text += " (`" + T.Value + "`)";
      Text += ".\n\n";
    }
  }
  for (auto I = Offset; I < End; ++I) {
    const auto &F = A.Findings[I];
    Text += "## " + std::to_string(I) + ". " +
            std::string(packageFindingKindName(F.Kind)) + "\n\n";
    const auto D = Effective.find(F.ID);
    Text += "Evidence: `" + F.Status + "`. Relationship: `" + F.Binding +
            "`. Confidence: `" + F.Confidence + "`.\n\n";
    Text +=
        "Disposition: `" +
        std::string(packageDispositionName(D == Effective.end()
                                               ? PackageDisposition::Unreviewed
                                               : D->second)) +
        "`. Source: `" +
        (D == Effective.end() ? "not_reviewed" : "caller_assertion") + "`.\n\n";
    Anchor("Finding", F.ID);
    Anchor("Owner", F.OwnerID);
    Anchor("Owner record", F.RecordID);
    Anchor("Package instance", F.PackageID);
    Anchor("Artifact", F.ArtifactID);
    Anchor("SHA-256", F.BlobHash);
    Anchor("Declaration artifact", F.DeclarationArtifactID);
    Anchor("Declaration SHA-256", F.DeclarationHash);
    Anchor("Digest algorithm", F.DigestAlgorithm);
    if (F.DigestAlgorithm == "sha1")
      Text += "Digest declaration uses legacy SHA-1.\n\n";
    Anchor("Source", F.SourceID);
    Anchor("Node", F.NodeID);
    Anchor("API class", F.API);
    Anchor("Pattern class", F.PatternKind);
    Anchor("Origin evidence", F.OriginEvidence);
    if (F.HasPatternCounts)
      Text += "Recognized syntax count before/after: " +
              std::to_string(F.BeforeCount) + "/" +
              std::to_string(F.AfterCount) + ".\n\n";
    if (F.HasSourceSpan)
      Text += "Source bytes: " + std::to_string(F.Start) + ".." +
              std::to_string(F.End) + ".\n\n";
    for (const auto &ID : F.RelatedIDs)
      Anchor("Related evidence", ID);
    if (Text.size() > ::neverd::limits::web::kReportMarkdownTextSizeLimit)
      throw Error("package_assessment_report_budget_exceeded");
  }
  return Text;
}
} // namespace neverd::web::package_assessment

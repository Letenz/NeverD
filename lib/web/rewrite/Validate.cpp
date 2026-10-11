//===- Validate.cpp - Local substitution and outer syntax validation ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve every unreplaced AST relationship, including ASI and directives.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

namespace neverd::web::rewrite {

void validate(SourceRewrite &A, const SourceAnalysis &Original,
              const SourceValueAnalysis &Values) {
  charge(A.Steps, A.Text.size());
  const auto Parsed =
      inspectJavaScript(identity("rewritten-source", {A.ID, A.CandidateHash}),
                        A.Text, A.SourceType);
  if (Parsed.ParseStatus != "parsed" || Parsed.LexemeStatus != "ok")
    throw Error("source_rewrite_reparse_failed");
  charge(A.Steps, validateSourceModel(Parsed));
  const auto CandidateValues = analyzeSourceValues(Parsed);
  if (CandidateValues.Nodes.size() != Parsed.Nodes.size() ||
      (CandidateValues.Status != "ok" && CandidateValues.Status != "partial"))
    throw Error("source_rewrite_candidate_values_unavailable");
  constexpr auto None = UINT32_MAX;
  std::vector<uint32_t> Replacements(Original.Nodes.size(), None);
  uint64_t Selected = 0;
  for (uint32_t I = 0; I < A.Records.size(); ++I) {
    charge(A.Steps);
    const auto &R = A.Records[I];
    if (R.Status != "accepted")
      continue;
    if (R.Node >= Original.Nodes.size() || Replacements[R.Node] != None ||
        Original.Nodes[R.Node].ID != R.NodeID ||
        Original.Nodes[R.Node].Start != R.SourceStart ||
        Original.Nodes[R.Node].End != R.SourceEnd)
      throw Error("source_rewrite_invalid_receipt");
    Replacements[R.Node] = I;
    ++Selected;
  }
  std::vector<std::pair<uint32_t, uint32_t>> Pending{{0, 0}};
  uint64_t Verified = 0;
  while (!Pending.empty()) {
    charge(A.Steps);
    const auto [Old, New] = Pending.back();
    Pending.pop_back();
    const auto &X = Original.Nodes.at(Old), &Y = Parsed.Nodes.at(New);
    if (Replacements[Old] != None) {
      const auto &R = A.Records[Replacements[Old]];
      const auto &Before = Values.Nodes.at(Old);
      const auto &After = CandidateValues.Nodes.at(New);
      if (Y.Start < R.Start || Y.End > R.End || Y.Start >= Y.End ||
          Before.Status != "constant" || After.Status != "constant" ||
          !Before.Value || !After.Value ||
          !sameValue(*Before.Value, *After.Value, A.Steps))
        throw Error("source_rewrite_value_mismatch");
      ++Verified;
      continue;
    }
    if (X.Kind != Y.Kind || X.Strict != Y.Strict ||
        X.Attributes.size() != Y.Attributes.size() ||
        X.Children.size() != Y.Children.size())
      throw Error("source_rewrite_outer_tree_mismatch");
    for (size_t I = 0; I < X.Attributes.size(); ++I) {
      charge(A.Steps);
      if (const auto *Text =
              std::get_if<std::u16string>(&X.Attributes[I].Value))
        charge(A.Steps, Text->size());
      if (const auto *Text =
              std::get_if<std::u16string>(&Y.Attributes[I].Value))
        charge(A.Steps, Text->size());
      if (X.Attributes[I].Field != Y.Attributes[I].Field ||
          X.Attributes[I].Value != Y.Attributes[I].Value)
        throw Error("source_rewrite_outer_tree_mismatch");
    }
    for (size_t I = 0; I < X.Children.size(); ++I) {
      charge(A.Steps);
      const auto &XC = X.Children[I], &YC = Y.Children[I];
      if (XC.Field != YC.Field || XC.Ordinal != YC.Ordinal)
        throw Error("source_rewrite_outer_tree_mismatch");
      Pending.emplace_back(XC.Index, YC.Index);
    }
  }
  if (Selected != A.Accepted || Verified != Selected)
    throw Error("source_rewrite_unverified_replacement");
}

} // namespace neverd::web::rewrite

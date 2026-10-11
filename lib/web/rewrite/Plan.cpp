//===- Plan.cpp - Bounded source rewrite selection ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Select explicit profiles and retain unknown expressions and original bytes.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceEffects.h"

#include <algorithm>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;

bool candidate(const SyntaxNode &N) {
  return N.Kind == "UnaryExpression" || N.Kind == "BinaryExpression" ||
         N.Kind == "TemplateLiteral";
}

struct Planner {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceValueAnalysis &V;
  const SourceEffectAnalysis &E;
  std::string_view Bytes;
  SourceRewrite &A;
  std::vector<uint32_t> Parents, Covered;
  std::vector<bool> Tagged;
  std::vector<std::pair<uint64_t, uint64_t>> Comments;

  void prepare() {
    rewrite::charge(A.Steps, validateSourceModel(S));
    if (S.LexemeStatus != "ok" || S.Lexemes.size() > MaxJavaScriptLexemes)
      throw Error("source_rewrite_lexemes_unavailable");
    if ((B.Status != "ok" && B.Status != "partial") ||
        (V.Status != "ok" && V.Status != "partial") ||
        (E.Status != "ok" && E.Status != "partial") ||
        V.Nodes.size() != S.Nodes.size() || E.Nodes.size() != S.Nodes.size())
      throw Error("source_rewrite_semantics_unavailable");
    for (const auto &Scope : B.Scopes) {
      rewrite::charge(A.Steps);
      if (Scope.PossibleDirectEval || Scope.ObjectEnvironment)
        throw Error("source_rewrite_dynamic_scope");
    }
    uint64_t Previous = 0;
    for (const auto &L : S.Lexemes) {
      rewrite::charge(A.Steps);
      if (L.Start < Previous || L.Start >= L.End || L.End > Bytes.size())
        throw Error("source_rewrite_invalid_lexemes");
      Previous = L.End;
      if (L.Kind == "comment" || L.Kind == "hashbang")
        Comments.emplace_back(L.Start, L.End);
    }
    Parents.assign(S.Nodes.size(), None);
    Covered.assign(S.Nodes.size(), None);
    Tagged.assign(S.Nodes.size(), false);
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      rewrite::charge(A.Steps);
      if (S.Nodes[I].End > Bytes.size())
        throw Error("source_rewrite_invalid_range");
      if (S.Nodes[I].Kind == "TaggedTemplateExpression")
        Tagged[I] = true;
      for (const auto &C : S.Nodes[I].Children) {
        rewrite::charge(A.Steps);
        Parents[C.Index] = I;
        Tagged[C.Index] = Tagged[I];
      }
    }
  }

  bool comment(uint64_t Start, uint64_t End) {
    size_t First = 0, Last = Comments.size();
    while (First != Last) {
      rewrite::charge(A.Steps);
      const auto Mid = First + (Last - First) / 2;
      if (Comments[Mid].second <= Start)
        First = Mid + 1;
      else
        Last = Mid;
    }
    return First != Comments.size() && Comments[First].first < End;
  }

  void run() {
    prepare();
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      rewrite::charge(A.Steps);
      if (Parents[I] != None)
        Covered[I] = Covered[Parents[I]];
      const auto &N = S.Nodes[I];
      if (!candidate(N))
        continue;
      if (A.Records.size() >= MaxSourceRewriteRecords)
        throw Error("source_rewrite_record_budget_exceeded");
      SourceRewriteRecord R;
      R.Node = I;
      R.NodeID = N.ID;
      R.SourceStart = N.Start;
      R.SourceEnd = N.End;
      R.Status = "refused";
      const auto &Value = V.Nodes[I];
      const auto &Effects = E.Nodes[I];
      if (Covered[I] != None) {
        R.Status = "covered";
        R.Reason = "outer_replacement";
      } else if (Tagged[I]) {
        R.Reason = "tagged_template_context";
      } else if (Value.Status != "constant" || !Value.Value) {
        R.Reason = Value.Status == "would_throw"
                       ? "expression_would_throw"
                       : "primitive_value_unavailable";
      } else if (Effects.Immediate || Effects.Deferred ||
                 Effects.ContainsDeclaration) {
        R.Reason = "effects_or_declarations";
      } else if ((Value.Value->Kind == PrimitiveKind::String) !=
                 (A.Pass == "string_recovery")) {
        R.Status = "not_selected";
        R.Reason = "different_value_kind";
      } else if (comment(N.Start, N.End)) {
        R.Reason = "comment_preservation";
      } else {
        R.ValueKind = sourcePrimitiveKindName(Value.Value->Kind);
        R.After = rewrite::primitive(*Value.Value, A.Steps);
        const auto Before = Bytes.substr(N.Start, N.End - N.Start);
        rewrite::charge(A.Steps, Before.size() + R.After.size());
        if (Before == R.After ||
            Before == std::string_view(R.After).substr(1, R.After.size() - 2)) {
          R.Status = "not_selected";
          R.Reason = "already_canonical";
          R.After.clear();
        } else {
          R.Before = Before;
          R.Status = "accepted";
          R.Reason = "constant_primitive_without_effects";
          Covered[I] = I;
          ++A.Accepted;
        }
      }
      A.Refused += R.Status == "refused";
      R.ID = identity("source-rewrite-record",
                      {A.ID, R.NodeID, R.Status, R.Reason});
      A.Records.push_back(std::move(R));
    }
  }
};
} // namespace

SourceRewrite rewriteJavaScript(std::string_view ArtifactID,
                                std::string_view Bytes,
                                std::string_view SourceType,
                                std::string_view Pass) {
  SourceRewrite A;
  try {
    const auto Profile = sourceRewriteProfile(Pass);
    if (Profile.empty())
      throw Error("unknown_source_rewrite_pass");
    if (Bytes.size() > MaxSourceRewriteBytes)
      throw Error("source_rewrite_byte_budget_exceeded");
    A.Pass = Pass;
    A.ArtifactID = ArtifactID;
    A.SourceType = SourceType;
    A.SourceSize = Bytes.size();
    rewrite::charge(A.Steps, Bytes.size());
    A.SourceHash = sha256(Bytes);
    const auto S = inspectJavaScript(ArtifactID, Bytes, SourceType);
    A.SourceID = S.ID;
    if (S.ParseStatus != "parsed")
      throw Error("source_rewrite_source_not_parsed");
    // A parsed AST can survive an exhausted lexer. Do not run downstream
    // models or infer comment absence without complete token evidence.
    if (S.LexemeStatus != "ok")
      throw Error("source_rewrite_lexemes_unavailable");
    const auto B = analyzeSourceBindings(S);
    A.BindingID = B.ID;
    if (Pass == "scope_renaming") {
      A.ID = identity("source-rewrite",
                      {S.ID, Profile, Pass, B.ID, A.ValueID, A.EffectID});
      rewrite::planRename(A, S, B, Bytes);
      rewrite::apply(A, Bytes);
      rewrite::validateRename(A, S, B);
      A.Reason = "complete_lexical_binding_projection_only";
    } else {
      const auto V = analyzeSourceValues(S);
      const auto E = analyzeSourceEffects(S, B, V);
      A.ValueID = V.ID;
      A.EffectID = E.ID;
      A.ID =
          identity("source-rewrite", {S.ID, Profile, Pass, B.ID, V.ID, E.ID});
      Planner{S, B, V, E, Bytes, A}.run();
      rewrite::apply(A, Bytes);
      rewrite::validate(A, S, V);
      A.Reason = "local_primitive_evaluation_only";
    }
    A.Status = A.Accepted ? "verified_local_rewrites" : "no_eligible_rewrites";
    // The inverse is checked before any candidate bytes are published.
    if (undoSourceRewrite(A, A.Text) != Bytes)
      throw Error("source_rewrite_inverse_mismatch");
  } catch (const Error &E) {
    A.Text.clear();
    A.CandidateHash.clear();
    A.Records.clear();
    A.Accepted = A.Refused = 0;
    A.Reason = E.what();
    A.Status = A.Reason.find("budget_exceeded") != std::string::npos
                   ? "budget_exceeded"
                   : "unavailable";
  }
  return A;
}

} // namespace neverd::web

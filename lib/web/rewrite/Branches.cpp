//===- Branches.cpp - Bounded Boolean expression selection --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Select proven Boolean branches without removing scopes or exposing refs.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <algorithm>

namespace neverd::web::rewrite {
namespace {
constexpr auto None = NoSourceIndex;
enum Barrier : unsigned { Declaration = 1, Scope = 2, Await = 4 };

struct Planner {
  SourceRewrite &A;
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceValueAnalysis &V;
  const SourceEffectAnalysis &E;
  std::string_view Bytes;
  std::vector<uint32_t> Parents, Covered;
  std::vector<unsigned> Barriers;
  std::vector<std::pair<uint64_t, uint64_t>> Comments;

  uint32_t child(uint32_t I, std::string_view Field) {
    for (const auto &C : S.Nodes[I].Children) {
      charge(A.Steps);
      if (C.Field == Field)
        return C.Index;
    }
    throw Error("source_branch_invalid_model");
  }

  void prepare() {
    charge(A.Steps, validateSourceModel(S));
    if (B.Status == "budget_exceeded" || V.Status == "budget_exceeded" ||
        E.Status == "budget_exceeded")
      throw Error("source_branch_semantic_budget_exceeded");
    if (B.Status != "ok" || B.SourceID != S.ID || V.SourceID != S.ID ||
        E.SourceID != S.ID || (V.Status != "ok" && V.Status != "partial") ||
        (E.Status != "ok" && E.Status != "partial") ||
        V.Nodes.size() != S.Nodes.size() || E.Nodes.size() != S.Nodes.size() ||
        B.NodeScopes.size() != S.Nodes.size())
      throw Error("source_branch_semantics_unavailable");
    if (S.LexemeStatus != "ok" || S.Lexemes.size() > MaxJavaScriptLexemes)
      throw Error("source_rewrite_lexemes_unavailable");
    Parents.assign(S.Nodes.size(), None);
    Covered.assign(S.Nodes.size(), None);
    Barriers.assign(S.Nodes.size(), 0);
    for (const auto &X : B.Scopes) {
      charge(A.Steps);
      if (X.PossibleDirectEval || X.PossibleEvalDeclarations ||
          X.ObjectEnvironment)
        throw Error("source_rewrite_dynamic_scope");
      if (X.Node != None) {
        if (X.Node >= S.Nodes.size())
          throw Error("source_branch_invalid_model");
        Barriers[X.Node] |= Scope;
      }
    }
    for (const auto &X : B.Bindings) {
      charge(A.Steps);
      if (X.Conflicting)
        throw Error("source_branch_semantics_unavailable");
    }
    for (const auto &D : B.Declarations) {
      charge(A.Steps);
      if (D.Node != None) {
        if (D.Node >= S.Nodes.size())
          throw Error("source_branch_invalid_model");
        Barriers[D.Node] |= Declaration;
      }
    }
    // The source model proves parent-before-child ownership. Summaries over
    // those edges need no assumption about contiguous preorder node indices.
    for (uint32_t I = S.Nodes.size(); I-- > 0;) {
      charge(A.Steps);
      const auto &N = S.Nodes[I];
      if (N.End > Bytes.size())
        throw Error("source_rewrite_invalid_range");
      if (N.Kind == "AwaitExpression")
        Barriers[I] |= Await;
      for (const auto &C : N.Children) {
        charge(A.Steps);
        Parents[C.Index] = I;
        Barriers[I] |= Barriers[C.Index];
      }
    }
    uint64_t Previous = 0;
    for (const auto &L : S.Lexemes) {
      charge(A.Steps);
      if (L.Start < Previous || L.Start >= L.End || L.End > Bytes.size())
        throw Error("source_rewrite_invalid_lexemes");
      Previous = L.End;
      if (L.Kind == "comment" || L.Kind == "hashbang")
        Comments.emplace_back(L.Start, L.End);
    }
  }

  bool comment(uint64_t Start, uint64_t End) {
    charge(A.Steps, searchSteps(Comments.size()));
    const auto It = std::lower_bound(Comments.begin(), Comments.end(), Start,
                                     [](const auto &Range, uint64_t Offset) {
                                       return Range.second <= Offset;
                                     });
    return It != Comments.end() && It->first < End;
  }

  void run() {
    prepare();
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      charge(A.Steps);
      if (Parents[I] != None)
        Covered[I] = Covered[Parents[I]];
      const auto &N = S.Nodes[I];
      const bool Conditional = N.Kind == "ConditionalExpression";
      if (!Conditional && N.Kind != "LogicalExpression")
        continue;
      if (A.Records.size() >= MaxSourceRewriteRecords)
        throw Error("source_rewrite_record_budget_exceeded");
      SourceRewriteRecord R;
      R.Node = I;
      R.NodeID = N.ID;
      R.SourceStart = N.Start;
      R.SourceEnd = N.End;
      R.Condition = child(I, Conditional ? "test" : "left");
      R.ConditionID = S.Nodes[R.Condition].ID;
      R.Status = "refused";
      const auto *Op = N.text("operator");
      const auto &Value = V.Nodes[R.Condition];
      const auto &Effects = E.Nodes[R.Condition];
      if (Covered[I] != None) {
        R.Status = "covered";
        R.Reason = "outer_replacement";
      } else if (!Conditional && (!Op || (*Op != u"&&" && *Op != u"||"))) {
        R.Reason = "unsupported_branch_operator";
      } else if (Barriers[I] & Await) {
        R.Reason = "await_syntax_preservation";
      } else if (Barriers[I] & (Scope | Declaration)) {
        R.Reason = "scope_or_declaration_preservation";
      } else if (Value.Status != "constant" || !Value.Value ||
                 Value.Value->Kind != PrimitiveKind::Boolean) {
        R.Reason = Value.Status == "would_throw"
                       ? "condition_would_throw"
                       : "constant_boolean_unavailable";
      } else if (Effects.Immediate || Effects.Deferred ||
                 Effects.ContainsDeclaration) {
        R.Reason = "condition_effects_or_declarations";
      } else if (comment(N.Start, N.End)) {
        R.Reason = "comment_preservation";
      } else {
        const bool Truth = Value.Value->Boolean;
        R.Selection =
            Conditional ? (Truth ? "consequent" : "alternate")
                        : ((*Op == u"&&" ? Truth : !Truth) ? "right" : "left");
        R.Retained = child(I, R.Selection);
        const auto &Chosen = S.Nodes[R.Retained];
        R.RetainedID = Chosen.ID;
        R.Before = Bytes.substr(N.Start, N.End - N.Start);
        charge(A.Steps, R.Before.size());
        // Never turn an evaluated value into a reference. Inner parentheses
        // keep a selected sequence as a single outer sequence operand.
        append(R.After, "(0,(", A.Steps);
        append(R.After, Bytes.substr(Chosen.Start, Chosen.End - Chosen.Start),
               A.Steps);
        append(R.After, "))", A.Steps);
        R.Status = "accepted";
        R.Reason = "constant_boolean_with_get_value";
        Covered[I] = I;
        ++A.Accepted;
      }
      A.Refused += R.Status == "refused";
      R.ID = branchRecordIdentity(A, R);
      A.Records.push_back(std::move(R));
    }
  }
};
} // namespace

std::string branchRecordIdentity(const SourceRewrite &A,
                                 const SourceRewriteRecord &R) {
  return identity("source-branch-record",
                  {A.ID, R.NodeID, R.ConditionID, R.RetainedID, R.Selection,
                   std::to_string(R.Node), std::to_string(R.Condition),
                   std::to_string(R.Retained), R.Status, R.Reason});
}

void planBranches(SourceRewrite &A, const SourceAnalysis &S,
                  const SourceBindingAnalysis &B, const SourceValueAnalysis &V,
                  const SourceEffectAnalysis &E, std::string_view Bytes) {
  Planner{A, S, B, V, E, Bytes}.run();
}
} // namespace neverd::web::rewrite

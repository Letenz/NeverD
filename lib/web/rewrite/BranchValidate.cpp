//===- BranchValidate.cpp - Retained branch and binding validation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify GetValue scaffolding, copied syntax and surviving lexical edges.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include <tuple>

namespace neverd::web::rewrite {
namespace {
constexpr auto None = NoSourceIndex;

void require(bool Condition) {
  if (!Condition)
    throw Error("source_branch_projection_mismatch");
}

struct Validator {
  SourceRewrite &A;
  const SourceAnalysis &Original, &Parsed;
  const SourceBindingAnalysis &Bindings, &Candidate;
  std::vector<uint32_t> Map, Replacements;
  std::vector<bool> Seen;

  uint32_t mapped(uint32_t Node) {
    if (Node == None)
      return None;
    require(Node < Map.size() && Map[Node] != None);
    return Map[Node];
  }

  void sameName(uint32_t Old, uint32_t New) {
    require(Old < Bindings.Names.size() && New < Candidate.Names.size());
    charge(A.Steps, Bindings.Names[Old].size() + Candidate.Names[New].size());
    require(Bindings.Names[Old] == Candidate.Names[New]);
  }

  void tree() {
    Map.assign(Original.Nodes.size(), None);
    Replacements.assign(Original.Nodes.size(), None);
    Seen.assign(Parsed.Nodes.size(), false);
    uint64_t Selected = 0;
    for (uint32_t I = 0; I < A.Records.size(); ++I) {
      charge(A.Steps);
      const auto &R = A.Records[I];
      if (R.Status != "accepted")
        continue;
      require(R.Node < Original.Nodes.size() &&
              R.Condition < Original.Nodes.size() &&
              R.Retained < Original.Nodes.size() &&
              Replacements[R.Node] == None);
      const auto &N = Original.Nodes[R.Node];
      require(N.ID == R.NodeID && N.Start == R.SourceStart &&
              N.End == R.SourceEnd &&
              Original.Nodes[R.Condition].ID == R.ConditionID &&
              Original.Nodes[R.Retained].ID == R.RetainedID &&
              R.ID == branchRecordIdentity(A, R));
      const auto Test = N.Kind == "ConditionalExpression" ? "test" : "left";
      bool HasTest = false, HasRetained = false;
      for (const auto &C : N.Children) {
        charge(A.Steps);
        HasTest |= C.Field == Test && C.Index == R.Condition;
        HasRetained |= C.Field == R.Selection && C.Index == R.Retained;
      }
      require((N.Kind == "ConditionalExpression" ||
               N.Kind == "LogicalExpression") &&
              HasTest && HasRetained);
      Replacements[R.Node] = I;
      ++Selected;
    }
    require(Selected == A.Accepted);

    std::vector<std::pair<uint32_t, uint32_t>> Pending{{0, 0}};
    uint64_t Verified = 0;
    while (!Pending.empty()) {
      charge(A.Steps);
      const auto [Old, New] = Pending.back();
      Pending.pop_back();
      require(Old < Map.size() && New < Seen.size() && !Seen[New]);
      Seen[New] = true;
      const auto &X = Original.Nodes[Old], &Y = Parsed.Nodes[New];
      if (Replacements[Old] != None) {
        const auto &R = A.Records[Replacements[Old]];
        require(Y.Kind == "SequenceExpression" && Y.Strict == X.Strict &&
                Y.Attributes.empty() && Y.Children.size() == 2 &&
                Y.Start >= R.Start && Y.End <= R.End);
        const auto &ZeroEdge = Y.Children[0], &ChosenEdge = Y.Children[1];
        require(ZeroEdge.Field == "expressions" && ZeroEdge.Ordinal == 0 &&
                ChosenEdge.Field == "expressions" && ChosenEdge.Ordinal == 1);
        const auto &Zero = Parsed.Nodes[ZeroEdge.Index];
        const auto &Chosen = Parsed.Nodes[ChosenEdge.Index];
        const auto &Retained = Original.Nodes[R.Retained];
        require(!Seen[ZeroEdge.Index] && Zero.Kind == "NumericLiteral" &&
                Zero.Children.empty() && Zero.Start == R.Start + 1 &&
                Zero.End == R.Start + 2 && Zero.Strict == X.Strict);
        const auto *Attribute = Zero.attribute("value");
        const auto *Number =
            Attribute ? std::get_if<double>(&Attribute->Value) : nullptr;
        require(Number && *Number == 0 &&
                A.Text.substr(Zero.Start, Zero.End - Zero.Start) == "0");
        Seen[ZeroEdge.Index] = true;
        require(Chosen.Start == R.Start + 4 && Chosen.End + 2 == R.End &&
                Chosen.End - Chosen.Start == Retained.End - Retained.Start &&
                R.After.size() == Retained.End - Retained.Start + 6 &&
                R.After.starts_with("(0,(") && R.After.ends_with("))"));
        charge(A.Steps, R.After.size());
        require(R.After.substr(4, Retained.End - Retained.Start) ==
                R.Before.substr(Retained.Start - R.SourceStart,
                                Retained.End - Retained.Start));
        Pending.emplace_back(R.Retained, ChosenEdge.Index);
        ++Verified;
        continue;
      }
      require(Map[Old] == None && X.Kind == Y.Kind && X.Strict == Y.Strict &&
              X.Attributes.size() == Y.Attributes.size() &&
              X.Children.size() == Y.Children.size());
      Map[Old] = New;
      for (size_t I = 0; I < X.Attributes.size(); ++I) {
        charge(A.Steps);
        const auto &XA = X.Attributes[I], &YA = Y.Attributes[I];
        if (const auto *Text = std::get_if<std::u16string>(&XA.Value))
          charge(A.Steps, Text->size());
        if (const auto *Text = std::get_if<std::u16string>(&YA.Value))
          charge(A.Steps, Text->size());
        require(XA.Field == YA.Field && XA.Value == YA.Value);
      }
      for (size_t I = 0; I < X.Children.size(); ++I) {
        charge(A.Steps);
        const auto &XC = X.Children[I], &YC = Y.Children[I];
        require(XC.Field == YC.Field && XC.Ordinal == YC.Ordinal);
        Pending.emplace_back(XC.Index, YC.Index);
      }
    }
    require(Verified == Selected);
    for (const bool Visited : Seen) {
      charge(A.Steps);
      require(Visited);
    }
  }

  void bindings() {
    require(Bindings.Scopes.size() == Candidate.Scopes.size() &&
            Bindings.Bindings.size() == Candidate.Bindings.size() &&
            Bindings.Declarations.size() == Candidate.Declarations.size() &&
            Bindings.NodeScopes.size() == Original.Nodes.size() &&
            Candidate.NodeScopes.size() == Parsed.Nodes.size());
    for (size_t I = 0; I < Bindings.Scopes.size(); ++I) {
      charge(A.Steps);
      const auto &X = Bindings.Scopes[I], &Y = Candidate.Scopes[I];
      require(mapped(X.Node) == Y.Node &&
              std::tie(X.Kind, X.Parent, X.VariableScope, X.Strict,
                       X.PerIteration, X.ObjectEnvironment,
                       X.PossibleEvalDeclarations, X.PossibleDirectEval) ==
                  std::tie(Y.Kind, Y.Parent, Y.VariableScope, Y.Strict,
                           Y.PerIteration, Y.ObjectEnvironment,
                           Y.PossibleEvalDeclarations, Y.PossibleDirectEval));
    }
    for (size_t I = 0; I < Bindings.Bindings.size(); ++I) {
      charge(A.Steps);
      const auto &X = Bindings.Bindings[I], &Y = Candidate.Bindings[I];
      require(std::tie(X.Kind, X.Scope, X.Conflicting, X.Immutable,
                       X.HasTemporalDeadZone, X.Implicit) ==
              std::tie(Y.Kind, Y.Scope, Y.Conflicting, Y.Immutable,
                       Y.HasTemporalDeadZone, Y.Implicit));
      sameName(X.Name, Y.Name);
    }
    for (size_t I = 0; I < Bindings.Declarations.size(); ++I) {
      charge(A.Steps);
      const auto &X = Bindings.Declarations[I], &Y = Candidate.Declarations[I];
      require(mapped(X.Node) == Y.Node &&
              std::tie(X.Binding, X.OccurrenceScope, X.Kind) ==
                  std::tie(Y.Binding, Y.OccurrenceScope, Y.Kind));
    }
    size_t Index = 0;
    for (const auto &X : Bindings.References) {
      charge(A.Steps);
      require(X.Node < Map.size());
      if (Map[X.Node] == None)
        continue;
      require(Index < Candidate.References.size());
      const auto &Y = Candidate.References[Index++];
      require(Map[X.Node] == Y.Node &&
              std::tie(X.Scope, X.Binding, X.Access, X.Resolution) ==
                  std::tie(Y.Scope, Y.Binding, Y.Access, Y.Resolution));
      sameName(X.Name, Y.Name);
    }
    require(Index == Candidate.References.size());
    for (uint32_t I = 0; I < Map.size(); ++I) {
      charge(A.Steps);
      if (Map[I] != None)
        require(Bindings.NodeScopes[I] == Candidate.NodeScopes[Map[I]]);
    }
  }
};
} // namespace

void validateBranches(SourceRewrite &A, const SourceAnalysis &Original,
                      const SourceBindingAnalysis &Bindings) {
  charge(A.Steps, A.Text.size());
  const auto Parsed =
      inspectJavaScript(identity("rewritten-source", {A.ID, A.CandidateHash}),
                        A.Text, A.SourceType);
  if (Parsed.ParseStatus != "parsed" || Parsed.LexemeStatus != "ok")
    throw Error("source_rewrite_reparse_failed");
  charge(A.Steps, validateSourceModel(Parsed));
  const auto Candidate = analyzeSourceBindings(Parsed);
  if (Candidate.Status == "budget_exceeded")
    throw Error("source_branch_binding_budget_exceeded");
  require(Bindings.Status == "ok" && Candidate.Status == "ok");
  Validator V{A, Original, Parsed, Bindings, Candidate};
  V.tree();
  V.bindings();
}
} // namespace neverd::web::rewrite

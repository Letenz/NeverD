//===- RenameValidate.cpp - Complete lexical rename validation ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reparse and compare all syntax, scopes, declarations and references.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceBindings.h"

#include <tuple>

namespace neverd::web::rewrite {
namespace {
constexpr auto None = NoSourceIndex;

void require(bool Condition) {
  if (!Condition)
    throw Error("source_rename_binding_mismatch");
}
} // namespace

void validateRename(SourceRewrite &A, const SourceAnalysis &Original,
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
    throw Error("source_rename_binding_budget_exceeded");
  require(Bindings.Status == "ok" && Candidate.Status == "ok");
  require(Original.Nodes.size() == Parsed.Nodes.size() &&
          Bindings.Bindings.size() == Candidate.Bindings.size() &&
          Bindings.Scopes.size() == Candidate.Scopes.size() &&
          Bindings.Declarations.size() == Candidate.Declarations.size() &&
          Bindings.References.size() == Candidate.References.size() &&
          Bindings.NodeScopes.size() == Candidate.NodeScopes.size());

  std::vector<uint32_t> Owners(Original.Nodes.size(), None);
  std::vector<std::string_view> Roles(Original.Nodes.size());
  auto Own = [&](uint32_t Node, uint32_t Binding, std::string_view Role) {
    charge(A.Steps);
    if (Node == None)
      return;
    require(Node < Owners.size() && Binding < Bindings.Bindings.size());
    if (Owners[Node] != None) {
      Roles[Node] = "ambiguous";
      return;
    }
    Owners[Node] = Binding;
    Roles[Node] = Role;
  };
  for (const auto &D : Bindings.Declarations)
    Own(D.Node, D.Binding, "declaration");
  for (const auto &R : Bindings.References)
    if (R.Binding != None)
      Own(R.Node, R.Binding, "reference");

  std::vector<uint32_t> Replacements(Original.Nodes.size(), None);
  std::vector<std::u16string> Renamed(Bindings.Bindings.size());
  uint64_t Selected = 0;
  for (uint32_t I = 0; I < A.Records.size(); ++I) {
    charge(A.Steps);
    const auto &R = A.Records[I];
    if (R.Status != "accepted")
      continue;
    require(R.Node < Original.Nodes.size() && Replacements[R.Node] == None);
    const auto Binding = Owners[R.Node];
    require(Binding != None && Roles[R.Node] == R.Role &&
            (R.Role == "declaration" || R.Role == "reference") &&
            R.BindingID == Bindings.Bindings[Binding].ID);
    const auto &N = Original.Nodes[R.Node];
    require(N.Kind == "Identifier" && N.ID == R.NodeID &&
            N.Start == R.SourceStart && N.End == R.SourceEnd &&
            !R.After.empty());
    charge(A.Steps, R.After.size());
    const std::u16string Name(R.After.begin(), R.After.end());
    if (Renamed[Binding].empty())
      Renamed[Binding] = Name;
    else
      require(Renamed[Binding] == Name);
    Replacements[R.Node] = I;
    ++Selected;
  }
  require(Selected == A.Accepted);

  for (uint32_t I = 0; I < Original.Nodes.size(); ++I) {
    charge(A.Steps);
    const auto &X = Original.Nodes[I], &Y = Parsed.Nodes[I];
    require(X.Kind == Y.Kind && X.Strict == Y.Strict &&
            X.Attributes.size() == Y.Attributes.size() &&
            X.Children.size() == Y.Children.size());
    bool ChangedName = false;
    for (size_t J = 0; J < X.Attributes.size(); ++J) {
      charge(A.Steps);
      const auto &Old = X.Attributes[J], &New = Y.Attributes[J];
      require(Old.Field == New.Field);
      if (const auto *Text = std::get_if<std::u16string>(&Old.Value))
        charge(A.Steps, Text->size());
      if (const auto *Text = std::get_if<std::u16string>(&New.Value))
        charge(A.Steps, Text->size());
      if (Replacements[I] != None && Old.Field == "name") {
        const auto &R = A.Records[Replacements[I]];
        const auto *Name = std::get_if<std::u16string>(&New.Value);
        require(Name && *Name == Renamed[Owners[I]] && Y.Start == R.Start &&
                Y.End == R.End);
        ChangedName = true;
      } else {
        require(Old.Value == New.Value);
      }
    }
    require(ChangedName == (Replacements[I] != None));
    for (size_t J = 0; J < X.Children.size(); ++J) {
      charge(A.Steps);
      const auto &XC = X.Children[J], &YC = Y.Children[J];
      require(std::tie(XC.Field, XC.Index, XC.Ordinal) ==
              std::tie(YC.Field, YC.Index, YC.Ordinal));
    }
  }

  for (size_t I = 0; I < Bindings.Scopes.size(); ++I) {
    charge(A.Steps);
    const auto &X = Bindings.Scopes[I], &Y = Candidate.Scopes[I];
    require(std::tie(X.Kind, X.Parent, X.Node, X.VariableScope, X.Strict,
                     X.PerIteration, X.ObjectEnvironment,
                     X.PossibleEvalDeclarations, X.PossibleDirectEval) ==
            std::tie(Y.Kind, Y.Parent, Y.Node, Y.VariableScope, Y.Strict,
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
    require(X.Name < Bindings.Names.size() && Y.Name < Candidate.Names.size());
    const auto &Expected =
        Renamed[I].empty() ? Bindings.Names[X.Name] : Renamed[I];
    charge(A.Steps, Expected.size() + Candidate.Names[Y.Name].size());
    require(Expected == Candidate.Names[Y.Name]);
  }
  for (size_t I = 0; I < Bindings.Declarations.size(); ++I) {
    charge(A.Steps);
    const auto &X = Bindings.Declarations[I], &Y = Candidate.Declarations[I];
    require(std::tie(X.Node, X.Binding, X.OccurrenceScope, X.Kind) ==
            std::tie(Y.Node, Y.Binding, Y.OccurrenceScope, Y.Kind));
  }
  for (size_t I = 0; I < Bindings.References.size(); ++I) {
    charge(A.Steps);
    const auto &X = Bindings.References[I], &Y = Candidate.References[I];
    require(std::tie(X.Node, X.Scope, X.Binding, X.Access, X.Resolution) ==
            std::tie(Y.Node, Y.Scope, Y.Binding, Y.Access, Y.Resolution));
    require(X.Name < Bindings.Names.size() && Y.Name < Candidate.Names.size());
    const auto &Expected = X.Binding != None && !Renamed[X.Binding].empty()
                               ? Renamed[X.Binding]
                               : Bindings.Names[X.Name];
    charge(A.Steps, Expected.size() + Candidate.Names[Y.Name].size());
    require(Expected == Candidate.Names[Y.Name]);
  }
  for (size_t I = 0; I < Bindings.NodeScopes.size(); ++I) {
    charge(A.Steps);
    require(Bindings.NodeScopes[I] == Candidate.NodeScopes[I]);
  }
}
} // namespace neverd::web::rewrite

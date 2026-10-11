//===- Rename.cpp - Bounded lexical identifier renaming -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Select complete local bindings while retaining name-dependent boundaries.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "RewriteInternal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceBindings.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web::rewrite {
namespace {
constexpr auto None = NoSourceIndex;

struct Planner {
  SourceRewrite &A;
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  std::string_view Bytes;
  std::vector<uint32_t> Parents;
  std::vector<std::vector<uint32_t>> Declarations, References;
  std::vector<std::string> Refusals;
  std::set<std::u16string> Names;
  std::map<std::pair<uint64_t, uint64_t>, uint32_t> IdentifierRanges;

  Planner(SourceRewrite &A, const SourceAnalysis &S,
          const SourceBindingAnalysis &B, std::string_view Bytes)
      : A(A), S(S), B(B), Bytes(Bytes) {}

  // A name search compares at most every character of its key per level.
  void chargeName(size_t Size) {
    charge(A.Steps, (Size + 1) * searchSteps(Names.size() + 1));
  }

  uint32_t child(uint32_t I, std::string_view Field) {
    for (const auto &C : S.Nodes[I].Children) {
      charge(A.Steps);
      if (C.Field == Field)
        return C.Index;
    }
    return None;
  }

  void prepare() {
    charge(A.Steps, validateSourceModel(S));
    if (B.Status == "budget_exceeded")
      throw Error("source_rename_binding_budget_exceeded");
    if (B.Status != "ok" || B.SourceID != S.ID)
      throw Error("source_rename_bindings_unavailable");
    if (S.LexemeStatus != "ok" || S.Lexemes.size() > MaxJavaScriptLexemes)
      throw Error("source_rewrite_lexemes_unavailable");
    for (const auto &Scope : B.Scopes) {
      charge(A.Steps);
      if (Scope.PossibleDirectEval || Scope.PossibleEvalDeclarations ||
          Scope.ObjectEnvironment)
        throw Error("source_rewrite_dynamic_scope");
    }
    uint64_t Previous = 0;
    for (const auto &L : S.Lexemes) {
      charge(A.Steps);
      if (L.Start < Previous || L.Start >= L.End || L.End > Bytes.size())
        throw Error("source_rewrite_invalid_lexemes");
      Previous = L.End;
    }
    Parents.assign(S.Nodes.size(), None);
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      charge(A.Steps);
      const auto &N = S.Nodes[I];
      if (N.End > Bytes.size())
        throw Error("source_rewrite_invalid_range");
      for (const auto &C : N.Children) {
        charge(A.Steps);
        Parents[C.Index] = I;
      }
      if (N.Kind == "Identifier") {
        const auto *Name = N.text("name");
        if (!Name)
          throw Error("source_rename_invalid_identifier");
        charge(A.Steps, Name->size());
        chargeName(Name->size());
        Names.insert(*Name);
        charge(A.Steps, searchSteps(IdentifierRanges.size() + 1));
        ++IdentifierRanges[{N.Start, N.End}];
      }
    }
    Declarations.resize(B.Bindings.size());
    References.resize(B.Bindings.size());
    Refusals.resize(B.Bindings.size());
    for (uint32_t I = 0; I < B.Declarations.size(); ++I) {
      charge(A.Steps);
      const auto &D = B.Declarations[I];
      if (D.Binding >= B.Bindings.size())
        throw Error("source_rename_invalid_binding");
      Declarations[D.Binding].push_back(I);
    }
    for (const auto &R : B.References) {
      charge(A.Steps);
      if (R.Binding == None)
        continue;
      if (R.Binding >= B.Bindings.size() || R.Node >= S.Nodes.size())
        throw Error("source_rename_invalid_binding");
      References[R.Binding].push_back(R.Node);
      if (R.Resolution != "lexical_binding")
        Refusals[R.Binding] = "uncertain_reference";
      else if (R.Access != "read")
        Refusals[R.Binding] = "binding_write_or_export";
    }
  }

  std::string functionBoundary(const SourceBinding &Binding) {
    if (Binding.Scope >= B.Scopes.size())
      throw Error("source_rename_invalid_binding");
    const auto Variable = B.Scopes[Binding.Scope].VariableScope;
    if (Variable >= B.Scopes.size())
      throw Error("source_rename_invalid_binding");
    const auto &Scope = B.Scopes[Variable];
    if (Scope.Kind == "function_variables")
      return "non_simple_parameter_environment";
    if (Scope.Kind != "function_parameters")
      return "non_function_binding";
    if (Scope.Node >= S.Nodes.size())
      throw Error("source_rename_invalid_binding");
    // Non-simple parameter environments can copy values into same-named body
    // variables. That implicit edge is not part of the lexical binding model.
    for (const auto &C : S.Nodes[Scope.Node].Children) {
      charge(A.Steps);
      if (C.Field == "params" && S.Nodes[C.Index].Kind != "Identifier")
        return "non_simple_parameter_environment";
    }
    return {};
  }

  bool exactIdentifier(uint32_t I) {
    if (I >= S.Nodes.size())
      return false;
    const auto &N = S.Nodes[I];
    if (N.Kind != "Identifier" || N.Start >= N.End)
      return false;
    charge(A.Steps, searchSteps(IdentifierRanges.size()));
    if (IdentifierRanges.at({N.Start, N.End}) != 1)
      return false;
    charge(A.Steps, searchSteps(S.Lexemes.size()));
    const auto L = std::lower_bound(
        S.Lexemes.begin(), S.Lexemes.end(), N.Start,
        [](const auto &L, uint64_t Offset) { return L.Start < Offset; });
    return L != S.Lexemes.end() && L->Kind == "identifier" &&
           L->Start == N.Start && L->End == N.End;
  }

  std::string admission(uint32_t I, uint32_t Declaration) {
    const auto &Binding = B.Bindings[I];
    if (Binding.Conflicting || Binding.Implicit || Declarations[I].size() != 1)
      return "ambiguous_or_implicit_declaration";
    if (Binding.Kind != "parameter" && Binding.Kind != "var" &&
        Binding.Kind != "let" && Binding.Kind != "const")
      return "name_sensitive_binding_kind";
    if (auto Reason = functionBoundary(Binding); !Reason.empty())
      return Reason;
    if (Binding.Name >= B.Names.size())
      throw Error("source_rename_invalid_binding");
    if (B.Names[Binding.Name] == u"arguments" ||
        B.Names[Binding.Name] == u"eval")
      return "special_identifier";
    if (!Refusals[I].empty())
      return Refusals[I];
    const auto Parent = Parents[Declaration];
    if (Parent == None)
      return "unsupported_declaration_form";
    const auto &P = S.Nodes[Parent];
    if (Binding.Kind == "parameter") {
      if (P.Kind != "FunctionDeclaration" && P.Kind != "FunctionExpression" &&
          P.Kind != "ArrowFunctionExpression")
        return "unsupported_declaration_form";
    } else {
      if (P.Kind != "VariableDeclarator" || child(Parent, "id") != Declaration)
        return "unsupported_declaration_form";
      const auto Init = child(Parent, "init");
      if (Init != None && (S.Nodes[Init].Kind == "FunctionExpression" ||
                           S.Nodes[Init].Kind == "ArrowFunctionExpression" ||
                           S.Nodes[Init].Kind == "ClassExpression"))
        return "inferred_function_or_class_name";
    }
    if (!exactIdentifier(Declaration))
      return "shared_or_unavailable_identifier_span";
    for (const auto Node : References[I]) {
      charge(A.Steps);
      if (!exactIdentifier(Node))
        return "shared_or_unavailable_identifier_span";
      const auto *Name = S.Nodes[Node].text("name");
      if (Name)
        charge(A.Steps, Name->size() + B.Names[Binding.Name].size());
      if (!Name || *Name != B.Names[Binding.Name])
        throw Error("source_rename_invalid_binding");
    }
    return {};
  }

  void record(uint32_t Binding, uint32_t Node, std::string_view Role,
              std::string_view Reason, std::string_view Name = {}) {
    charge(A.Steps);
    if (A.Records.size() >= MaxSourceRewriteRecords)
      throw Error("source_rewrite_record_budget_exceeded");
    const auto &N = S.Nodes[Node];
    SourceRewriteRecord R;
    R.Node = Node;
    R.NodeID = N.ID;
    R.BindingID = B.Bindings[Binding].ID;
    R.Role = Role;
    R.SourceStart = N.Start;
    R.SourceEnd = N.End;
    R.Status = Name.empty() ? "refused" : "accepted";
    R.Reason = Reason;
    if (!Name.empty()) {
      R.Before = Bytes.substr(N.Start, N.End - N.Start);
      R.After = Name;
      charge(A.Steps, R.Before.size() + R.After.size());
      ++A.Accepted;
    } else {
      ++A.Refused;
    }
    R.ID = identity("source-rename-record",
                    {A.ID, R.BindingID, R.NodeID, R.Role, R.Status, R.Reason});
    A.Records.push_back(std::move(R));
  }

  void run() {
    prepare();
    for (uint32_t I = 0; I < B.Bindings.size(); ++I) {
      charge(A.Steps);
      if (Declarations[I].empty())
        continue;
      const auto Declaration = B.Declarations[Declarations[I][0]].Node;
      if (Declaration == None)
        continue;
      if (Declaration >= S.Nodes.size())
        throw Error("source_rename_invalid_binding");
      if (auto Reason = admission(I, Declaration); !Reason.empty()) {
        record(I, Declaration, "declaration", Reason);
        continue;
      }
      std::string Name =
          B.Bindings[I].Kind == "parameter" ? "neverd_arg_" : "neverd_local_";
      Name += std::to_string(I);
      for (;;) {
        charge(A.Steps, Name.size());
        chargeName(Name.size());
        if (Names.insert(std::u16string(Name.begin(), Name.end())).second)
          break;
        Name += '_';
      }
      record(I, Declaration, "declaration", "complete_lexical_binding", Name);
      for (const auto Node : References[I])
        record(I, Node, "reference", "complete_lexical_binding", Name);
    }
  }
};
} // namespace

void planRename(SourceRewrite &A, const SourceAnalysis &S,
                const SourceBindingAnalysis &B, std::string_view Bytes) {
  Planner(A, S, B, Bytes).run();
}
} // namespace neverd::web::rewrite

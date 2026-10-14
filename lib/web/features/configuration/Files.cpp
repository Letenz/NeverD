//===- Files.cpp - Configuration file and layer syntax navigation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize fixed literal sites and retain minified lexical source
/// relationships.
///
//===----------------------------------------------------------------------===//

#include "../../SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/ConfigurationFiles.h"
#include "neverd/web/Error.h"

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
struct Spelling {
  std::u16string_view Text;
  std::string_view Role;
  bool Layer;
};
constexpr Spelling Vocabulary[] = {
    {u"settings.json", "settings_file", false},
    {u"settings.local.json", "local_settings_file", false},
    {u"managed-settings.json", "managed_settings_file", false},
    {u"managed-settings.d", "managed_settings_directory", false},
    {u"userSettings", "user_layer", true},
    {u"projectSettings", "project_layer", true},
    {u"localSettings", "local_layer", true},
    {u"flagSettings", "flag_layer", true},
    {u"policySettings", "policy_layer", true}};

struct Context {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceNavigation &N;
  ConfigurationFileNavigation A;
  SourceFunctionIndex Functions;
  std::vector<uint32_t> Parents, Enclosing, Cases, Calls;
  std::vector<std::string_view> Fields;
  std::vector<bool> CandidateFunctions;

  void step(uint64_t Count = 1) {
    if (Count > sourceWorkLimit(S, MaxConfigurationFileSteps) - A.Steps)
      throw Error("configuration_file_work_budget_exceeded");
    A.Steps += Count;
  }
  void record() {
    step();
    if (A.Sites.size() + A.Calls.size() + A.Sequences.size() >=
        MaxConfigurationFileRecords)
      throw Error("configuration_file_record_budget_exceeded");
  }
  const Spelling *spelling(uint32_t Node) {
    step();
    const auto &Value = S.Nodes[Node];
    if (Value.Kind != "StringLiteral")
      return nullptr;
    const auto *Text = Value.text("value");
    if (!Text)
      return nullptr;
    for (const auto &Entry : Vocabulary) {
      step();
      if (*Text == Entry.Text)
        return &Entry;
    }
    return nullptr;
  }
  void prepare() {
    step(validateSourceModel(S));
    if (N.SourceID != S.ID || N.BindingID != B.ID || B.SourceID != S.ID ||
        (N.Status != "ok" && N.Status != "partial") ||
        (B.Status != "ok" && B.Status != "partial") ||
        N.Calls.size() > S.Nodes.size() ||
        B.References.size() > sourceLimits(S).Nodes)
      throw Error("configuration_file_evidence_unavailable");
    Functions = indexSourceFunctions(S, B, N);
    step(Functions.Steps);
    const auto Size = S.Nodes.size();
    Parents.assign(Size, None);
    Enclosing.assign(Size, None);
    Cases.assign(Size, None);
    Calls.assign(Size, None);
    Fields.resize(Size);
    CandidateFunctions.assign(N.Functions.size(), false);
    for (uint32_t I = 0; I < N.Calls.size(); ++I) {
      step();
      const auto &Call = N.Calls[I];
      if (Call.Node >= Size || Calls[Call.Node] != None ||
          (Call.Reference != None && Call.Reference >= B.References.size()) ||
          (Call.Callee != None && Call.Callee >= Size) ||
          (Call.SyntacticFunction != None &&
           Call.SyntacticFunction >= N.Functions.size()))
        throw Error("invalid_source_navigation_model");
      Calls[Call.Node] = I;
    }
    for (uint32_t I = 0; I < Size; ++I) {
      step();
      auto Function = Enclosing[I], Case = Cases[I];
      if (Functions.NodeFunctions[I] != None) {
        Function = Functions.NodeFunctions[I];
        Case = None;
      }
      if (S.Nodes[I].Kind == "SwitchCase")
        Case = I;
      for (const auto &Child : S.Nodes[I].Children) {
        step();
        Parents[Child.Index] = I;
        Fields[Child.Index] = Child.Field;
        Enclosing[Child.Index] = Function;
        Cases[Child.Index] = Case;
      }
    }
  }
  void site(uint32_t Node, const Spelling &Entry) {
    const auto Parent = Parents[Node];
    if (Parent == None)
      return;
    const auto &P = S.Nodes[Parent];
    std::string Kind;
    uint32_t Call = None;
    if ((P.Kind == "CallExpression" || P.Kind == "OptionalCallExpression") &&
        Fields[Node] == "arguments") {
      Kind = Entry.Layer ? "layer_call_argument" : "file_call_argument";
      Call = Calls[Parent];
      if (Call == None)
        throw Error("invalid_source_navigation_model");
    } else if (Entry.Layer && P.Kind == "SwitchCase" &&
               Fields[Node] == "test") {
      Kind = "layer_case_test";
    } else if (Entry.Layer && P.Kind == "ArrayExpression" &&
               Fields[Node] == "elements") {
      Kind = "layer_array_element";
    } else if (!Entry.Layer && P.Kind == "Property" &&
               Fields[Node] == "value" && !P.flag("method") && P.text("kind") &&
               *P.text("kind") == u"init") {
      Kind = "file_property_value";
    } else if (!Entry.Layer && P.Kind == "ReturnStatement" &&
               Fields[Node] == "argument") {
      Kind = "file_return_literal";
    } else {
      return;
    }
    record();
    ConfigurationFileSite Site;
    Site.ID = identity("configuration-file-site", {A.ID, S.Nodes[Node].ID});
    Site.Node = Node;
    Site.Kind = std::move(Kind);
    Site.Role = Entry.Role;
    Site.Function = Enclosing[Node];
    Site.Call = Call;
    Site.Case = Cases[Node];
    if (Site.Function != None)
      CandidateFunctions[Site.Function] = true;
    A.Sites.push_back(std::move(Site));
  }
  void sequence(uint32_t Node) {
    const auto &Array = S.Nodes[Node];
    uint32_t Layers = 0;
    for (const auto &Child : Array.Children) {
      step();
      if (Child.Field == "elements") {
        const auto *Entry = spelling(Child.Index);
        Layers += Entry && Entry->Layer;
      }
    }
    if (Layers < 2)
      return;
    record();
    ConfigurationLayerSequence Sequence;
    Sequence.Node = Node;
    Sequence.Function = Enclosing[Node];
    Sequence.ID = identity("configuration-layer-sequence", {A.ID, Array.ID});
    for (const auto &Child : Array.Children) {
      step();
      if (Child.Field != "elements")
        continue;
      if (Child.Ordinal != Sequence.Terms.size())
        throw Error("configuration_file_invalid_sequence");
      if (A.Terms >= MaxConfigurationFileTerms)
        throw Error("configuration_file_term_budget_exceeded");
      ++A.Terms;
      const auto *Entry = spelling(Child.Index);
      Sequence.Terms.push_back(
          {Child.Index, Child.Ordinal,
           Entry && Entry->Layer ? std::string(Entry->Role) : std::string{}});
    }
    A.Sequences.push_back(std::move(Sequence));
  }
  void call(uint32_t Index) {
    step();
    const auto &Call = N.Calls[Index];
    if (Call.Kind != "call" && Call.Kind != "optional_call")
      return;
    auto Target = Call.SyntacticFunction;
    if (Call.Reference != None) {
      const auto &Ref = B.References[Call.Reference];
      if (Ref.Node != Call.Callee ||
          (Ref.Binding != None && Ref.Binding >= B.Bindings.size()))
        throw Error("invalid_source_binding_model");
      if (Ref.Resolution == "lexical_binding" && Ref.Binding != None)
        Target = Functions.BindingFunctions[Ref.Binding];
    }
    if (Target == None || !CandidateFunctions[Target])
      return;
    record();
    A.Calls.push_back(
        {identity("configuration-file-call", {A.ID, Call.ID}), Index, Target});
  }
};
} // namespace

ConfigurationFileNavigation
analyzeConfigurationFiles(const SourceAnalysis &S,
                          const SourceBindingAnalysis &B,
                          const SourceNavigation &N) {
  Context C{S, B, N};
  C.A.ID = identity("configuration-file-navigation",
                    {S.ID, B.ID, N.ID, ConfigurationFilesProfile});
  C.A.SourceID = S.ID;
  C.A.BindingID = B.ID;
  C.A.NavigationID = N.ID;
  try {
    C.prepare();
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      C.step();
      if (const auto *Entry = C.spelling(I))
        C.site(I, *Entry);
      if (S.Nodes[I].Kind == "ArrayExpression")
        C.sequence(I);
    }
    for (uint32_t I = 0; I < N.Calls.size(); ++I)
      C.call(I);
    C.A.Status = "partial";
  } catch (const Error &E) {
    C.A.Sites.clear();
    C.A.Calls.clear();
    C.A.Sequences.clear();
    C.A.Terms = 0;
    C.A.Reason = E.what();
    C.A.Status = C.A.Reason.ends_with("budget_exceeded") ? "budget_exceeded"
                                                         : "unavailable";
  }
  return std::move(C.A);
}
} // namespace neverd::web

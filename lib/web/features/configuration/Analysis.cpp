//===- Analysis.cpp - Ordered configuration syntax terms ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compose existing feature/binding evidence without propagating runtime
/// values.
///
//===----------------------------------------------------------------------===//

#include "../../SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/ConfigurationOrder.h"
#include "neverd/web/Error.h"
#include "neverd/web/SourceNavigation.h"

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
constexpr uint32_t role(ConfigurationField Field) {
  return static_cast<uint32_t>(Field);
}
struct Context {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceNavigation &N;
  const SourceFeatures &F;
  ConfigurationSourceOrder A;
  std::vector<uint32_t> Parents, References, Initializers, CallFeatures;
  std::vector<uint32_t> Roles;
  void step(uint64_t Count = 1) {
    if (Count > sourceWorkLimit(S, MaxConfigurationOrderSteps) - A.Steps)
      throw Error("configuration_order_work_budget_exceeded");
    A.Steps += Count;
  }
  uint32_t child(uint32_t Node, std::string_view Field) {
    for (const auto &C : S.Nodes.at(Node).Children) {
      step();
      if (C.Field == Field)
        return C.Index;
    }
    return None;
  }
  const std::u16string *key(uint32_t Node, bool Computed) {
    if (Node == None)
      return nullptr;
    step();
    const auto &N = S.Nodes[Node];
    return N.Kind == "StringLiteral"             ? N.text("value")
           : !Computed && N.Kind == "Identifier" ? N.text("name")
                                                 : nullptr;
  }
  void prepare() {
    step(validateSourceModel(S));
    if (B.SourceID != S.ID || N.SourceID != S.ID || N.BindingID != B.ID ||
        F.SourceID != S.ID || F.NavigationID != N.ID || F.Status != "partial" ||
        (N.Status != "ok" && N.Status != "partial") ||
        (B.Status != "ok" && B.Status != "partial") ||
        F.Records.size() > MaxJavaScriptClientFeatureRecords ||
        B.References.size() > S.Nodes.size() ||
        B.Bindings.size() > 2 * sourceLimits(S).Nodes)
      throw Error("configuration_order_evidence_unavailable");
    Parents.assign(S.Nodes.size(), None);
    References.assign(S.Nodes.size(), None);
    Roles.assign(S.Nodes.size(), None);
    CallFeatures.assign(S.Nodes.size(), None);
    Initializers.assign(B.Bindings.size(), None);
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      step();
      for (const auto &C : S.Nodes[I].Children) {
        step();
        Parents[C.Index] = I;
      }
    }
    for (uint32_t I = 0; I < B.References.size(); ++I) {
      step();
      const auto &R = B.References[I];
      if (R.Node >= S.Nodes.size() || References[R.Node] != None ||
          S.Nodes[R.Node].Kind != "Identifier" ||
          (R.Binding != None && R.Binding >= B.Bindings.size()))
        throw Error("configuration_order_invalid_bindings");
      References[R.Node] = I;
    }
    const auto Declarations = indexSourceDeclarations(S, B);
    step(Declarations.Steps);
    std::vector<uint32_t> DeclarationCounts(B.Bindings.size());
    for (const auto &D : B.Declarations) {
      step();
      ++DeclarationCounts[D.Binding];
    }
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      step();
      if (S.Nodes[I].Kind != "VariableDeclarator")
        continue;
      const auto ID = child(I, "id"), Init = child(I, "init");
      if (ID == None || S.Nodes[ID].Kind != "Identifier")
        continue;
      const auto Binding = Declarations.UniqueBindings[ID];
      if (Binding == None || DeclarationCounts[Binding] != 1)
        continue;
      Initializers[Binding] = Init;
    }
    for (uint32_t I = 0; I < F.Records.size(); ++I) {
      step();
      const auto &R = F.Records[I];
      if (R.Node >= S.Nodes.size() || R.Trigger >= S.Nodes.size())
        throw Error("configuration_order_invalid_features");
      if (R.Pattern == SourceFeaturePattern::ConfigurationCall) {
        if (S.Nodes[R.Node].Kind != "CallExpression" &&
            S.Nodes[R.Node].Kind != "OptionalCallExpression")
          throw Error("configuration_order_invalid_features");
        CallFeatures[R.Node] = I;
        continue;
      }
      if (R.Pattern != SourceFeaturePattern::ConfigurationSection &&
          R.Pattern != SourceFeaturePattern::PermissionSection &&
          R.Pattern != SourceFeaturePattern::MCPSection)
        continue;
      if (S.Nodes[R.Node].Kind != "Property" ||
          child(R.Node, "key") != R.Trigger || Parents[R.Node] == None ||
          S.Nodes[Parents[R.Node]].Kind != "ObjectExpression")
        throw Error("configuration_order_invalid_features");
      const auto *K = key(R.Trigger, S.Nodes[R.Node].flag("computed"));
      if (!K)
        throw Error("configuration_order_invalid_features");
      const auto Role =
          *K == u"permissions"      ? role(ConfigurationField::Permissions)
          : *K == u"permissionMode" ? role(ConfigurationField::PermissionMode)
          : *K == u"mcpServers"     ? role(ConfigurationField::MCPServers)
          : *K == u"hooks"          ? role(ConfigurationField::Hooks)
                                    : None;
      if (Role == None || Roles[R.Node] != None)
        throw Error("configuration_order_invalid_features");
      Roles[R.Node] = Role;
    }
  }
  void object(uint32_t Node) {
    ConfigurationObjectOrder O;
    O.Node = Node;
    O.ID = identity("source-config-object-order", {A.ID, S.Nodes[Node].ID});
    for (const auto &C : S.Nodes[Node].Children) {
      step();
      if (C.Field != "properties")
        continue;
      if (A.Terms == MaxConfigurationOrderTerms)
        throw Error("configuration_order_term_budget_exceeded");
      ++A.Terms;
      ConfigurationTerm T;
      T.Node = C.Index;
      const auto &N = S.Nodes[C.Index];
      if (N.Kind == "SpreadElement") {
        T.Kind = "object_spread";
        T.Value = child(C.Index, "argument");
        T.UnknownKeys = true;
      } else if (N.Kind == "Property") {
        const auto K = child(C.Index, "key");
        T.UnknownKeys = !key(K, N.flag("computed"));
        const auto *Kind = N.text("kind");
        T.Kind = !Kind || *Kind != u"init" || N.flag("method")
                     ? "accessor_or_method_property"
                 : T.UnknownKeys ? "computed_property"
                                 : "static_property";
        T.Value = child(C.Index, "value");
      } else {
        throw Error("configuration_order_invalid_property");
      }
      if (T.UnknownKeys)
        for (auto &Field : O.Fields)
          Field.LaterUnknown = true;
      if (Roles[C.Index] != None) {
        auto &Field = O.Fields[Roles[C.Index]];
        Field.Seen = true;
        Field.LastDefinition = C.Index;
        Field.LaterUnknown = false;
        ++Field.DefinitionCount;
      }
      if (T.Value != None && S.Nodes[T.Value].Kind == "Identifier") {
        T.Reference = References[T.Value];
        if (T.Reference != None) {
          const auto &R = B.References[T.Reference];
          if (R.Resolution == "lexical_binding" && R.Binding != None)
            T.Initializer = Initializers[R.Binding];
        }
      }
      const auto Origin = T.Initializer != None ? T.Initializer : T.Value;
      if (Origin != None)
        T.Feature = CallFeatures[Origin];
      T.ID = identity("source-config-order-term",
                      {O.ID, N.ID, std::to_string(O.Terms.size())});
      O.Terms.push_back(std::move(T));
    }
    if (A.Objects.size() == MaxConfigurationOrderRecords)
      throw Error("configuration_order_record_budget_exceeded");
    A.Objects.push_back(std::move(O));
  }
};
} // namespace
ConfigurationSourceOrder analyzeConfigurationSourceOrder(
    const SourceAnalysis &S, const SourceBindingAnalysis &B,
    const SourceNavigation &N, const SourceFeatures &F) {
  Context C{S, B, N, F};
  C.A.ID = identity("source-configuration-order",
                    {S.ID, B.ID, N.ID, F.ID, ConfigurationOrderProfile});
  C.A.SourceID = S.ID;
  C.A.NavigationID = N.ID;
  C.A.FeatureID = F.ID;
  try {
    C.prepare();
    std::vector<bool> Objects(S.Nodes.size());
    for (uint32_t I = 0; I < C.Roles.size(); ++I) {
      C.step();
      if (C.Roles[I] != None)
        Objects[C.Parents[I]] = true;
    }
    for (uint32_t I = 0; I < Objects.size(); ++I) {
      C.step();
      if (Objects[I])
        C.object(I);
    }
    C.A.Status = "partial";
  } catch (const Error &E) {
    C.A.Objects.clear();
    C.A.Terms = 0;
    C.A.Reason = E.what();
    C.A.Status = C.A.Reason.ends_with("budget_exceeded") ? "budget_exceeded"
                                                         : "unavailable";
  }
  return std::move(C.A);
}
} // namespace neverd::web

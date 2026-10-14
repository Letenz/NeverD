//===- Source.cpp - Bounded desktop bridge syntax analysis ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize direct member/message shapes without evaluating or executing JS.
/// Shared bindings/navigation own lexical resolution and source containment.
///
//===----------------------------------------------------------------------===//

#include "../../SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/DesktopBridge.h"
#include "neverd/web/Error.h"

#include <algorithm>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
struct Context {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceNavigation &N;
  DesktopBridgeSource A;
  std::vector<uint32_t> References;
  void step(uint64_t Count = 1) {
    if (Count > sourceWorkLimit(S, MaxDesktopBridgeSteps) - A.Steps)
      throw Error("desktop_bridge_work_budget_exceeded");
    A.Steps += Count;
  }
  uint32_t child(uint32_t Index, std::string_view Field) {
    if (Index == None)
      return None;
    for (const auto &C : S.Nodes.at(Index).Children) {
      step();
      if (C.Field == Field)
        return C.Index;
    }
    return None;
  }
  const std::u16string *key(uint32_t Index, bool Computed) {
    if (Index == None)
      return nullptr;
    step();
    const auto &Node = S.Nodes.at(Index);
    return Node.Kind == "StringLiteral"             ? Node.text("value")
           : !Computed && Node.Kind == "Identifier" ? Node.text("name")
                                                    : nullptr;
  }
  std::vector<uint32_t> chain(uint32_t Index, DesktopBridgeCall &R) {
    std::vector<uint32_t> Names;
    while (Index != None) {
      step();
      const auto &Node = S.Nodes.at(Index);
      if (Node.Kind == "Identifier") {
        if (Node.text("name") && *Node.text("name") == u"window") {
          R.Root = Index;
          std::reverse(Names.begin(), Names.end());
          return Names;
        }
        break;
      }
      if (Names.size() == 5 || (Node.Kind != "MemberExpression" &&
                                Node.Kind != "OptionalMemberExpression"))
        break;
      const auto Property = child(Index, "property");
      if (!key(Property, Node.flag("computed")))
        break;
      R.Optional |=
          Node.Kind == "OptionalMemberExpression" || Node.flag("optional");
      Names.push_back(Property);
      Index = child(Index, "object");
    }
    return {};
  }
  bool name(uint32_t Index, std::u16string_view Value) {
    const auto *K = key(Index, false);
    return K && *K == Value;
  }
  void message(DesktopBridgeCall &R) {
    if (R.Payload == None || S.Nodes[R.Payload].Kind != "ObjectExpression") {
      R.Reason = "message_not_direct_object";
      return;
    }
    for (const auto &C : S.Nodes[R.Payload].Children) {
      step();
      if (C.Field != "properties")
        continue;
      const auto &P = S.Nodes[C.Index];
      if (P.Kind != "Property") {
        R.Reason = "message_spread_or_unknown_property";
        continue;
      }
      const auto K = child(C.Index, "key");
      const auto *Text = key(K, P.flag("computed"));
      if (!Text) {
        R.Reason = "message_computed_property";
        continue;
      }
      uint32_t *Slot = *Text == u"cmd"        ? &R.Command
                       : *Text == u"callback" ? &R.Callback
                       : *Text == u"error"    ? &R.Error
                                              : nullptr;
      if (!Slot)
        continue;
      if (*Slot != None)
        R.Reason = "message_duplicate_role_property";
      const auto *Kind = P.text("kind");
      if (!Kind || *Kind != u"init" || P.flag("method"))
        R.Reason = "message_role_accessor_or_method";
      *Slot = child(C.Index, "value");
    }
    if (R.Command == None && R.Reason.empty())
      R.Reason = "message_command_missing";
  }
  void call(uint32_t Index) {
    step();
    const auto &Call = N.Calls[Index];
    if (Call.Kind != "call" && Call.Kind != "optional_call")
      return;
    DesktopBridgeCall R;
    R.Node = Call.Node;
    R.Call = Index;
    R.Optional =
        Call.Kind == "optional_call" || S.Nodes.at(Call.Node).flag("optional");
    const auto Names = chain(Call.Callee, R);
    if (Names.size() == 1 && name(Names[0], u"__TAURI_IPC__")) {
      R.Family = "tauri_1_candidate";
      R.Form = "window_ipc_message";
    } else if (Names.size() == 3 && name(Names[0], u"__TAURI__") &&
               name(Names[1], u"tauri") && name(Names[2], u"invoke")) {
      R.Family = "tauri_1_candidate";
      R.Form = "window_global_invoke";
    } else if (Names.size() == 4 && name(Names[0], u"go")) {
      R.Family = "wails_2_candidate";
      R.Form = "window_go_package_struct_method";
      std::copy(Names.begin() + 1, Names.end(), R.TargetNames.begin());
    } else {
      return;
    }
    if (Call.Node >= S.Nodes.size() || R.Root >= References.size())
      throw Error("desktop_bridge_invalid_navigation");
    R.RootReference = References[R.Root];
    R.RootResolution = R.RootReference == None
                           ? "unavailable"
                           : B.References[R.RootReference].Resolution;
    std::vector<uint32_t> Arguments;
    for (const auto &C : S.Nodes[Call.Node].Children) {
      step();
      if (C.Field == "arguments") {
        Arguments.push_back(C.Index);
        R.Spread |= S.Nodes[C.Index].Kind == "SpreadElement";
      }
    }
    R.Arguments = Arguments.size();
    if (R.Spread) {
      R.Reason = "spread_arguments";
    } else if (R.Form == "window_ipc_message") {
      if (Arguments.size() != 1)
        R.Reason = "ipc_message_arity_unavailable";
      else {
        R.Payload = Arguments[0];
        message(R);
      }
    } else if (R.Form == "window_global_invoke") {
      if (Arguments.empty() || Arguments.size() > 2)
        R.Reason = "invoke_arity_unavailable";
      else {
        R.Command = Arguments[0];
        if (Arguments.size() == 2)
          R.Payload = Arguments[1];
      }
    }
    R.Status = R.Reason.empty() ? "matched_syntax" : "unresolved_shape";
    R.CommandStatus = R.Form == "window_go_package_struct_method"
                          ? "static_member_names_redacted"
                      : !R.Reason.empty() ? "unresolved_argument_shape"
                      : R.Command == None ? "unavailable"
                      : S.Nodes[R.Command].Kind == "StringLiteral"
                          ? "literal_string_redacted"
                          : "dynamic_expression";
    if (A.Calls.size() == MaxDesktopBridgeRecords)
      throw Error("desktop_bridge_record_budget_exceeded");
    R.ID =
        identity("desktop-source-bridge", {A.ID, S.Nodes[R.Node].ID, R.Form});
    A.Calls.push_back(std::move(R));
  }
  void prepare() {
    step(validateSourceModel(S));
    if (N.SourceID != S.ID || N.BindingID != B.ID || B.SourceID != S.ID ||
        (N.Status != "ok" && N.Status != "partial") ||
        (B.Status != "ok" && B.Status != "partial") ||
        N.Calls.size() > S.Nodes.size() || B.References.size() > S.Nodes.size())
      throw Error("desktop_bridge_evidence_unavailable");
    References.assign(S.Nodes.size(), None);
    std::vector<bool> CallNodes(S.Nodes.size());
    for (const auto &C : N.Calls) {
      step();
      if (C.Node >= S.Nodes.size() || CallNodes[C.Node] ||
          (C.EnclosingFunction != None &&
           C.EnclosingFunction >= N.Functions.size()))
        throw Error("desktop_bridge_invalid_navigation");
      const std::string_view Expected =
          C.Kind == "call"              ? "CallExpression"
          : C.Kind == "optional_call"   ? "OptionalCallExpression"
          : C.Kind == "construct"       ? "NewExpression"
          : C.Kind == "tagged_template" ? "TaggedTemplateExpression"
          : C.Kind == "dynamic_import"  ? "ImportExpression"
                                        : "";
      const bool Import = C.Kind == "dynamic_import";
      const auto Target =
          child(C.Node, Import                        ? "source"
                        : C.Kind == "tagged_template" ? "tag"
                                                      : "callee");
      if (Expected.empty() || S.Nodes[C.Node].Kind != Expected ||
          Target == None || Target != (Import ? C.Argument : C.Callee) ||
          (Import && C.Callee != None))
        throw Error("desktop_bridge_invalid_navigation");
      CallNodes[C.Node] = true;
    }
    for (uint32_t I = 0; I < B.References.size(); ++I) {
      step();
      const auto Node = B.References[I].Node;
      if (Node >= References.size() || References[Node] != None ||
          S.Nodes[Node].Kind != "Identifier" ||
          (B.References[I].Binding != None &&
           B.References[I].Binding >= B.Bindings.size()))
        throw Error("desktop_bridge_invalid_bindings");
      References[Node] = I;
    }
  }
};
} // namespace
DesktopBridgeSource analyzeDesktopBridgeSource(const SourceAnalysis &S,
                                               const SourceBindingAnalysis &B,
                                               const SourceNavigation &N) {
  Context C{S, B, N};
  C.A.ID = identity("desktop-source-bridges",
                    {S.ID, B.ID, N.ID, DesktopBridgeProfile});
  C.A.SourceID = S.ID;
  C.A.NavigationID = N.ID;
  try {
    C.prepare();
    for (uint32_t I = 0; I < N.Calls.size(); ++I)
      C.call(I);
    C.A.Status = "partial";
  } catch (const Error &E) {
    C.A.Calls.clear();
    C.A.Reason = E.what();
    C.A.Status = C.A.Reason == "desktop_bridge_work_budget_exceeded" ||
                         C.A.Reason == "desktop_bridge_record_budget_exceeded"
                     ? "budget_exceeded"
                     : "unavailable";
  }
  return std::move(C.A);
}
} // namespace neverd::web

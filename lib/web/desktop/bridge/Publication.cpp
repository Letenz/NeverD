//===- Publication.cpp - Desktop bridge anchors and coverage --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retain static/unknown distinctions; never publish command or argument data.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::desktop_bridge {
llvm::json::Object summary(const DesktopBridgeSource &A) {
  uint64_t Unresolved = 0, NonExternal = 0;
  for (const auto &R : A.Calls) {
    Unresolved += R.Status == "unresolved_shape";
    NonExternal += R.RootResolution != "external";
  }
  return llvm::json::Object{
      {"profile", std::string(DesktopBridgeProfile)},
      {"analysis_id", A.ID},
      {"source_id", A.SourceID},
      {"navigation_id", A.NavigationID},
      {"status", A.Status},
      {"reason", A.Reason.empty() ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(A.Reason)},
      {"record_count", A.Calls.size()},
      {"unresolved_shape_count", Unresolved},
      {"non_external_root_count", NonExternal},
      {"steps", A.Steps},
      {"pattern_inventory_complete", A.Status == "partial"},
      {"bridge_inventory_complete", false},
      {"api_identity", "not_authenticated"},
      {"native_target", "not_resolved"},
      {"runtime_activation", "not_checked"},
      {"property_mutation_and_getters", "not_analyzed"},
      {"commands_and_arguments_redacted", true},
      {"executes_input", false}};
}
llvm::json::Object record(const DesktopBridgeCall &R, const SourceAnalysis &S,
                          const SourceBindingAnalysis &B,
                          const SourceNavigation &N) {
  auto NodeID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(S.Nodes.at(I).ID);
  };
  const auto &Call = N.Calls.at(R.Call);
  const auto &Node = S.Nodes.at(R.Node);
  const auto *Ref = R.RootReference == NoSourceIndex
                        ? nullptr
                        : &B.References.at(R.RootReference);
  llvm::json::Array Names;
  for (const auto I : R.TargetNames)
    if (I != NoSourceIndex)
      Names.emplace_back(NodeID(I));
  return llvm::json::Object{
      {"bridge_id", R.ID},
      {"family_candidate", R.Family},
      {"form", R.Form},
      {"recognition_status", R.Status},
      {"reason", R.Reason.empty() ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(R.Reason)},
      {"confidence", "source_syntax_only"},
      {"node_id", Node.ID},
      {"byte_offset", std::to_string(Node.Start)},
      {"byte_length", std::to_string(Node.End - Node.Start)},
      {"call_id", Call.ID},
      {"callee_node_id", NodeID(Call.Callee)},
      {"enclosing_function_id",
       Call.EnclosingFunction == NoSourceIndex
           ? llvm::json::Value(nullptr)
           : llvm::json::Value(N.Functions.at(Call.EnclosingFunction).ID)},
      {"root_node_id", NodeID(R.Root)},
      {"root_reference_id",
       Ref ? llvm::json::Value(Ref->ID) : llvm::json::Value(nullptr)},
      {"root_binding_id",
       Ref && Ref->Binding != NoSourceIndex
           ? llvm::json::Value(B.Bindings.at(Ref->Binding).ID)
           : llvm::json::Value(nullptr)},
      {"root_resolution", R.RootResolution},
      {"command_node_id", NodeID(R.Command)},
      {"command_status", R.CommandStatus},
      {"payload_node_id", NodeID(R.Payload)},
      {"callback_value_node_id", NodeID(R.Callback)},
      {"error_value_node_id", NodeID(R.Error)},
      {"target_name_node_ids", std::move(Names)},
      {"argument_count", R.Arguments},
      {"spread_arguments", R.Spread},
      {"optional", R.Optional},
      {"native_target", "not_resolved"},
      {"runtime_callback_target", "not_proven"},
      {"api_identity", "not_authenticated"},
      {"commands_and_arguments_redacted", true}};
}
} // namespace neverd::web::desktop_bridge

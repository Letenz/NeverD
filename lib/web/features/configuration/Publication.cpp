//===- Publication.cpp - Redacted configuration source relationships ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe source definitions and possible overwrite barriers, never policy.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::configuration_order {
llvm::json::Object summary(const ConfigurationSourceOrder &A) {
  return llvm::json::Object{
      {"profile", std::string(ConfigurationOrderProfile)},
      {"analysis_id", A.ID},
      {"source_id", A.SourceID},
      {"navigation_id", A.NavigationID},
      {"feature_analysis_id", A.FeatureID},
      {"status", A.Status},
      {"reason", A.Reason.empty() ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(A.Reason)},
      {"object_count", A.Objects.size()},
      {"term_count", A.Terms},
      {"steps", A.Steps},
      {"pattern_inventory_complete", A.Status == "partial"},
      {"configuration_inventory_complete", false},
      {"precedence_basis", "object_property_source_order"},
      {"successful_object_creation", "not_checked"},
      {"effective_configuration", "not_analyzed"},
      {"file_layer_precedence", "not_analyzed"},
      {"initializer_link", "lexical_source_only"},
      {"configuration_loader_identity", "not_authenticated"},
      {"mutation_getters_and_proxies", "not_analyzed"},
      {"configuration_values_redacted", true},
      {"executes_input", false}};
}
llvm::json::Object record(const ConfigurationObjectOrder &O,
                          const SourceAnalysis &S,
                          const SourceBindingAnalysis &B,
                          const SourceFeatures &F) {
  auto NodeID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(S.Nodes.at(I).ID);
  };
  llvm::json::Array Terms, Fields;
  for (uint32_t I = 0; I < O.Terms.size(); ++I) {
    const auto &T = O.Terms[I];
    const auto &N = S.Nodes.at(T.Node);
    const auto *Ref =
        T.Reference == NoSourceIndex ? nullptr : &B.References.at(T.Reference);
    Terms.emplace_back(llvm::json::Object{
        {"term_id", T.ID},
        {"ordinal", I},
        {"kind", T.Kind},
        {"node_id", N.ID},
        {"byte_offset", std::to_string(N.Start)},
        {"byte_length", std::to_string(N.End - N.Start)},
        {"value_node_id", NodeID(T.Value)},
        {"unknown_keys", T.UnknownKeys},
        {"reference_id",
         Ref ? llvm::json::Value(Ref->ID) : llvm::json::Value(nullptr)},
        {"binding_id", Ref && Ref->Binding != NoSourceIndex
                           ? llvm::json::Value(B.Bindings.at(Ref->Binding).ID)
                           : llvm::json::Value(nullptr)},
        {"binding_resolution", Ref ? Ref->Resolution : "not_applicable"},
        {"initializer_node_id", NodeID(T.Initializer)},
        {"configuration_call_feature_id",
         T.Feature == NoSourceIndex
             ? llvm::json::Value(nullptr)
             : llvm::json::Value(F.Records.at(T.Feature).ID)},
        {"runtime_value", "not_proven"}});
  }
  constexpr std::string_view Names[] = {"permissions", "permissionMode",
                                        "mcpServers", "hooks"};
  for (uint32_t I = 0; I < O.Fields.size(); ++I) {
    const auto &Field = O.Fields[I];
    if (!Field.Seen)
      continue;
    Fields.emplace_back(llvm::json::Object{
        {"field_role", std::string(Names[I])},
        {"definition_count", Field.DefinitionCount},
        {"last_source_definition_node_id", NodeID(Field.LastDefinition)},
        {"possible_later_overwrite", Field.LaterUnknown},
        {"precedence_status",
         Field.LaterUnknown ? "later_unknown_term" : "last_direct_definition"},
        {"effective_value", "not_proven"}});
  }
  const auto &Node = S.Nodes.at(O.Node);
  return llvm::json::Object{
      {"object_order_id", O.ID},
      {"node_id", Node.ID},
      {"byte_offset", std::to_string(Node.Start)},
      {"byte_length", std::to_string(Node.End - Node.Start)},
      {"confidence", "source_syntax_only"},
      {"terms", std::move(Terms)},
      {"fields", std::move(Fields)},
      {"effective_configuration", "not_analyzed"},
      {"configuration_values_redacted", true}};
}
} // namespace neverd::web::configuration_order

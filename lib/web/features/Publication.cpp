//===- Publication.cpp - Client source pattern metadata ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish fixed vocabulary, opaque identities and exact source anchors only.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

#include "neverd/web/SourceNavigation.h"

#include <array>

namespace neverd::web::client_features {
namespace {
constexpr std::string_view domain(SourceFeatureDomain D) {
  switch (D) {
  case SourceFeatureDomain::Tool:
    return "tool";
  case SourceFeatureDomain::Configuration:
    return "configuration";
  case SourceFeatureDomain::Permission:
    return "permission";
  case SourceFeatureDomain::MCP:
    return "mcp";
  case SourceFeatureDomain::Hook:
    return "hook";
  }
  llvm_unreachable("Closed source feature domain");
}
constexpr std::string_view pattern(SourceFeaturePattern P) {
  switch (P) {
  case SourceFeaturePattern::ToolDefinition:
    return "tool_definition_shape";
  case SourceFeaturePattern::ToolRegistrationCall:
    return "tool_registration_named_call";
  case SourceFeaturePattern::ToolDispatchCall:
    return "tool_dispatch_named_call";
  case SourceFeaturePattern::ConfigurationCall:
    return "configuration_named_call";
  case SourceFeaturePattern::ConfigurationSection:
    return "configuration_section_property";
  case SourceFeaturePattern::PermissionCall:
    return "permission_named_call";
  case SourceFeaturePattern::PermissionSection:
    return "permission_setting_property";
  case SourceFeaturePattern::MCPCall:
    return "mcp_named_call";
  case SourceFeaturePattern::MCPSection:
    return "mcp_settings_property";
  case SourceFeaturePattern::HookCall:
    return "hook_named_call";
  case SourceFeaturePattern::HookEvent:
    return "hook_event_property";
  case SourceFeaturePattern::HookEventArgument:
    return "hook_event_literal_argument";
  }
  llvm_unreachable("Closed source feature pattern");
}
} // namespace

llvm::json::Object summary(const SourceFeatures &A) {
  constexpr SourceFeatureDomain Domains[] = {
      SourceFeatureDomain::Tool, SourceFeatureDomain::Configuration,
      SourceFeatureDomain::Permission, SourceFeatureDomain::MCP,
      SourceFeatureDomain::Hook};
  std::array<uint64_t, 5> Counts{}, Unresolved{};
  for (const auto &R : A.Records) {
    ++Counts[unsigned(R.Domain)];
    Unresolved[unsigned(R.Domain)] += R.Status == "unresolved_shape";
  }
  llvm::json::Array Coverage, Diagnostics;
  const bool Available = A.Status == "partial";
  for (const auto D : Domains) {
    const auto I = unsigned(D);
    Coverage.emplace_back(
        llvm::json::Object{{"domain", std::string(domain(D))},
                           {"status", !Available  ? "unavailable"
                                      : Counts[I] ? "syntax_candidates"
                                                  : "no_pattern_match"},
                           {"record_count", Counts[I]},
                           {"unresolved_shape_count", Unresolved[I]},
                           {"semantic_attribution", "not_proven"}});
  }
  for (const auto &D : A.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{{"code", D.Code}});
  return llvm::json::Object{
      {"profile", std::string(JavaScriptClientFeatureProfile)},
      {"analysis_id",
       A.ID.empty() ? llvm::json::Value(nullptr) : llvm::json::Value(A.ID)},
      {"status", A.Status},
      {"record_count", A.Records.size()},
      {"coverage", std::move(Coverage)},
      {"computed_member_call_count", A.ComputedMemberCalls},
      {"steps", A.Steps},
      {"diagnostics", std::move(Diagnostics)},
      {"pattern_inventory_complete", Available},
      {"feature_inventory_complete", false},
      {"api_identity", "not_authenticated"},
      {"runtime_targets", "not_proven"},
      {"effective_policy", "not_analyzed"},
      {"configuration_precedence", "not_analyzed"},
      {"server_behavior", "not_analyzed"},
      {"source_values_redacted", true}};
}

llvm::json::Object record(const SourceFeature &R, const SourceAnalysis &S,
                          const SourceBindingAnalysis &B,
                          const SourceNavigation &N) {
  auto NodeID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(S.Nodes.at(I).ID);
  };
  auto FunctionID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(N.Functions.at(I).ID);
  };
  const auto &Node = S.Nodes.at(R.Node);
  return llvm::json::Object{
      {"feature_id", R.ID},
      {"domain", std::string(domain(R.Domain))},
      {"pattern", std::string(pattern(R.Pattern))},
      {"recognition_basis", R.Basis},
      {"recognition_status", R.Status},
      {"reason", R.Reason.empty() ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(R.Reason)},
      {"confidence", "source_syntax_only"},
      {"node_id", Node.ID},
      {"trigger_node_id", NodeID(R.Trigger)},
      {"byte_offset", std::to_string(Node.Start)},
      {"byte_length", std::to_string(Node.End - Node.Start)},
      {"syntax_kind", Node.Kind},
      {"enclosing_function_id", FunctionID(R.Function)},
      {"call_id", R.Call == NoSourceIndex
                      ? llvm::json::Value(nullptr)
                      : llvm::json::Value(N.Calls.at(R.Call).ID)},
      {"name_node_id", NodeID(R.Name)},
      {"name_status", R.NameStatus},
      {"callback_candidate_node_id", NodeID(R.Callback)},
      {"callback_binding_id",
       R.CallbackBinding == NoSourceIndex
           ? llvm::json::Value(nullptr)
           : llvm::json::Value(B.Bindings.at(R.CallbackBinding).ID)},
      {"callback_source_function_id", FunctionID(R.CallbackFunction)},
      {"callback_source_link", R.Callback == NoSourceIndex ? "not_applicable"
                               : R.CallbackFunction == NoSourceIndex
                                   ? "unresolved"
                               : R.CallbackBinding == NoSourceIndex
                                   ? "function_expression_syntax"
                                   : "lexical_declaration_or_initializer"},
      {"condition_node_id", NodeID(R.Condition)},
      {"condition_relationship", R.Condition == NoSourceIndex
                                     ? "not_in_if_or_conditional_test"
                                     : "source_containment_in_test"},
      {"argument_count", R.Arguments},
      {"argument_ordinal", R.ArgumentOrdinal == NoSourceIndex
                               ? llvm::json::Value(nullptr)
                               : llvm::json::Value(R.ArgumentOrdinal)},
      {"spread_arguments", R.SpreadArguments},
      {"runtime_callback_target", "not_proven"},
      {"source_values_redacted", true}};
}

} // namespace neverd::web::client_features

//===- FilesPublication.cpp - Configuration source metadata --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish admitted syntax evidence with explicit source-only relationships.
///
//===----------------------------------------------------------------------===//

#include "FilesPublication.h"

namespace neverd::web::configuration_files {
llvm::json::Object summary(const ConfigurationFileNavigation &A) {
  return llvm::json::Object{
      {"profile", std::string(ConfigurationFilesProfile)},
      {"analysis_id", A.ID},
      {"source_id", A.SourceID},
      {"binding_analysis_id", A.BindingID},
      {"navigation_id", A.NavigationID},
      {"status", A.Status},
      {"reason", A.Reason.empty() ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(A.Reason)},
      {"record_count", A.Sites.size() + A.Calls.size() + A.Sequences.size()},
      {"literal_site_count", A.Sites.size()},
      {"lexical_call_count", A.Calls.size()},
      {"layer_sequence_count", A.Sequences.size()},
      {"layer_term_count", A.Terms},
      {"steps", A.Steps},
      {"pattern_inventory_complete", A.Status == "partial"},
      {"configuration_inventory_complete", false},
      {"function_link", "lexical_source_only"},
      {"sequence_basis", "array_element_source_order"},
      {"file_layer_precedence", "not_analyzed"},
      {"effective_configuration", "not_analyzed"},
      {"runtime_targets", "not_proven"},
      {"configuration_values_redacted", true},
      {"executes_input", false}};
}

llvm::json::Object record(const ConfigurationFileNavigation &A,
                          const SourceAnalysis &S, const SourceNavigation &N,
                          uint64_t Index) {
  auto NodeID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(S.Nodes.at(I).ID);
  };
  auto FunctionID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(N.Functions.at(I).ID);
  };
  llvm::json::Object Row;
  uint32_t Node;
  if (Index < A.Sites.size()) {
    const auto &Site = A.Sites[Index];
    Node = Site.Node;
    Row = llvm::json::Object{
        {"record_id", Site.ID},
        {"record_type", "literal_site"},
        {"site_kind", Site.Kind},
        {"role", Site.Role},
        {"enclosing_function_id", FunctionID(Site.Function)},
        {"call_id", Site.Call == NoSourceIndex
                        ? llvm::json::Value(nullptr)
                        : llvm::json::Value(N.Calls.at(Site.Call).ID)},
        {"switch_case_node_id", NodeID(Site.Case)},
        {"case_relation", "source_containment_only"}};
  } else if (Index - A.Sites.size() < A.Calls.size()) {
    const auto &Link = A.Calls[Index - A.Sites.size()];
    const auto &Call = N.Calls.at(Link.Call);
    Node = Call.Node;
    Row = llvm::json::Object{
        {"record_id", Link.ID},
        {"record_type", "lexical_call"},
        {"call_id", Call.ID},
        {"enclosing_function_id", FunctionID(Call.EnclosingFunction)},
        {"candidate_function_id", FunctionID(Link.TargetFunction)},
        {"candidate_function_node_id",
         NodeID(N.Functions.at(Link.TargetFunction).Node)},
        {"function_link", "lexical_source_only"},
        {"runtime_target", "not_proven"}};
  } else {
    const auto &Sequence =
        A.Sequences.at(Index - A.Sites.size() - A.Calls.size());
    Node = Sequence.Node;
    llvm::json::Array Terms;
    for (const auto &Term : Sequence.Terms) {
      const auto &T = S.Nodes.at(Term.Node);
      Terms.emplace_back(llvm::json::Object{
          {"ordinal", Term.Ordinal},
          {"node_id", T.ID},
          {"syntax_kind", T.Kind},
          {"byte_offset", std::to_string(T.Start)},
          {"byte_length", std::to_string(T.End - T.Start)},
          {"role", Term.Role.empty() ? llvm::json::Value(nullptr)
                                     : llvm::json::Value(Term.Role)},
          {"status", Term.Role.empty() ? "unknown_element" : "literal_layer"}});
    }
    Row = llvm::json::Object{
        {"record_id", Sequence.ID},
        {"record_type", "layer_sequence"},
        {"enclosing_function_id", FunctionID(Sequence.Function)},
        {"terms", std::move(Terms)},
        {"sequence_basis", "array_element_source_order"},
        {"file_layer_precedence", "not_analyzed"}};
  }
  const auto &Original = S.Nodes.at(Node);
  Row["node_id"] = Original.ID;
  Row["syntax_kind"] = Original.Kind;
  Row["byte_offset"] = std::to_string(Original.Start);
  Row["byte_length"] = std::to_string(Original.End - Original.Start);
  Row["confidence"] = "source_syntax_only";
  Row["configuration_values_redacted"] = true;
  return Row;
}
} // namespace neverd::web::configuration_files

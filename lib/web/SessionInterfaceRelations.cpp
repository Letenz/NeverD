//===- SessionInterfaceRelations.cpp - Request relation publication ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit selected request analyses, separate caches and metadata pages.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
namespace {
llvm::json::Object summary(const InterfaceSourceRelations &A,
                           uint64_t Revision) {
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"relations_id", A.ID},
      {"interface_analysis_id", A.InterfaceID},
      {"source_id", A.SourceID},
      {"binding_analysis_id", A.BindingID},
      {"navigation_id", A.NavigationID},
      {"profile", std::string(InterfaceSourceRelationProfile)},
      {"analysis_status", A.Status},
      {"reason", A.Reason},
      {"relation_count", A.Records.size()},
      {"steps", A.Steps},
      {"evidence_class", "static_inference"},
      {"relationship_meaning", "source_syntax_only"},
      {"result_value_identity", "not_verified"},
      {"response_api_identity", "member_spelling_unverified"},
      {"wrapper_argument_flow", "not_analyzed"},
      {"callback_parameter_roles", "not_assigned"},
      {"response_transformations", "direct_argument_syntax_only"},
      {"runtime_call_graph", "not_analyzed"},
      {"reachability", "not_analyzed"},
      {"request_ordering", "not_analyzed"},
      {"har_correlation", false},
      {"source_execution_observed", false},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeInterfaceRelations(std::string_view Revision,
                                               std::string_view InterfaceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Interface = State->InterfaceSources.find(std::string(InterfaceID));
  if (Interface == State->InterfaceSources.end())
    throw Error("unknown_interface_analysis");
  for (const auto &[ID, Relations] : State->InterfaceRelations)
    if (Relations.InterfaceID == InterfaceID)
      return json(summary(Relations, State->Revision));
  if (State->InterfaceRelations.size() >= 4)
    throw Error("interface_relation_cache_budget_exceeded");
  const auto &I = Interface->second;
  const auto &S = State->Sources.at(I.SourceID);
  const auto &B = State->Bindings.at(I.SourceID);
  // Reuse a published index when present. A local index has the same identity
  // and work charge, without creating client-feature results as a side effect.
  const auto Existing = State->Navigation.find(S.ID);
  const auto Local = Existing == State->Navigation.end()
                         ? web::analyzeSourceNavigation(S, B)
                         : SourceNavigation{};
  const auto &N =
      Existing == State->Navigation.end() ? Local : Existing->second.Evidence;
  auto A = web::analyzeInterfaceSourceRelations(S, B, I, N);
  auto Reply = json(summary(A, State->Revision));
  State->InterfaceRelations.emplace(A.ID, std::move(A));
  return Reply;
#endif
}

std::string Session::interfaceRelationRecords(std::string_view Revision,
                                              std::string_view RelationsID,
                                              uint64_t Offset,
                                              uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Found = State->InterfaceRelations.find(std::string(RelationsID));
  if (Found == State->InterfaceRelations.end())
    throw Error("unknown_interface_relations");
  const auto &A = Found->second;
  const auto &S = State->Sources.at(A.SourceID);
  const auto &B = State->Bindings.at(A.SourceID);
  const auto &I = State->InterfaceSources.at(A.InterfaceID);
  if (!Limit ||
      Limit >
          ::neverd::limits::web::
              kSessionInterfaceRelationsSessionInterfaceRelationRecordsLimit ||
      Offset > A.Records.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(A.Records.size(), Offset + Limit);
  auto Node = [&](uint32_t Index) {
    return Index == NoSourceIndex ? llvm::json::Value(nullptr)
                                  : llvm::json::Value(S.Nodes.at(Index).ID);
  };
  llvm::json::Array Items;
  for (auto Index = Offset; Index < End; ++Index) {
    const auto &R = A.Records[Index];
    const auto &At = S.Nodes.at(R.Node);
    Items.emplace_back(llvm::json::Object{
        {"relation_id", R.ID},
        {"kind", R.Kind},
        {"interface_id", I.Records.at(R.Request).ID},
        {"request_node_id", Node(I.Records.at(R.Request).Node)},
        {"node_id", Node(R.Node)},
        {"source_byte_offset", std::to_string(At.Start)},
        {"source_byte_length", std::to_string(At.End - At.Start)},
        {"function_node_id", Node(R.FunctionNode)},
        {"receiver_node_id", Node(R.ReceiverNode)},
        {"input_node_id", Node(R.InputNode)},
        {"syntax_containment_depth", R.ContainmentDepth},
        {"binding_id", R.Binding == NoSourceIndex
                           ? llvm::json::Value(nullptr)
                           : llvm::json::Value(B.Bindings.at(R.Binding).ID)},
        {"member_spelling", R.MemberKind},
        {"arguments_status", R.ArgumentsStatus},
        {"handler_node_id", Node(R.HandlerNode)},
        {"handler_status", R.HandlerStatus}});
  }
  auto Reply = summary(A, State->Revision);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == A.Records.size();
  Reply["next_offset"] = End == A.Records.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}

} // namespace neverd::web

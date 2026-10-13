//===- RelationContext.cpp - Request relationship admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded shared-model admission, lexical indexes and opaque identities.
///
//===----------------------------------------------------------------------===//

#include "RelationContext.h"

#include "../SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

namespace neverd::web::interface_relations {

void Context::step(uint64_t Count) {
  if (Count > Limits.Steps - A.Steps)
    throw Error("interface_relation_work_budget_exceeded");
  A.Steps += Count;
}

uint32_t Context::child(uint32_t Node, std::string_view Field,
                        uint32_t Ordinal) {
  if (Node == None)
    return None;
  for (const auto &C : S.Nodes.at(Node).Children) {
    step();
    if (C.Field == Field && C.Ordinal == Ordinal)
      return C.Index;
  }
  return None;
}

void Context::prepare() {
  if (Limits.Steps > sourceWorkLimit(S, MaxInterfaceRelationSteps) ||
      Limits.Records > MaxInterfaceSourceRelations)
    throw Error("invalid_interface_relation_limits");
  step(validateSourceModel(S));
  if (I.SourceID != S.ID || I.BindingID != B.ID || B.SourceID != S.ID ||
      N.SourceID != S.ID || N.BindingID != B.ID ||
      I.ID != identity("source-interfaces",
                       {S.ID, B.ID, I.ValueID, SourceInterfaceProfile}) ||
      N.ID != identity("source-navigation",
                       {S.ID, JavaScriptNavigationProfile, B.ID, B.Status}))
    throw Error("interface_relation_evidence_mismatch");
  if (I.Status != "partial")
    throw Error("interface_relation_request_evidence_unavailable");
  if ((N.Status != "ok" && N.Status != "partial") ||
      (B.Status != "ok" && B.Status != "partial"))
    throw Error("interface_relation_navigation_unavailable");
  // Cached navigation is charged exactly as freshly computed navigation.
  step(N.Steps);
  const auto Size = S.Nodes.size();
  if (I.Records.size() > MaxInterfaceRecords || N.Calls.size() > Size ||
      N.Functions.size() > Size || B.Bindings.size() > sourceLimits(S).Nodes ||
      B.References.size() > sourceLimits(S).Nodes ||
      B.Scopes.size() > sourceLimits(S).Nodes)
    throw Error("invalid_interface_relation_model");
  Parents.assign(Size, None);
  Calls.assign(Size, None);
  Requests.assign(Size, None);
  References.assign(Size, None);
  ResultBindings.assign(B.Bindings.size(), None);
  WrittenBindings.assign(B.Bindings.size(), false);
  auto Index = indexSourceDeclarations(S, B);
  step(Index.Steps);
  Declarations = std::move(Index.UniqueBindings);
  for (uint32_t Node = 0; Node < Size; ++Node) {
    step();
    for (const auto &C : S.Nodes[Node].Children) {
      step();
      Parents[C.Index] = Node;
    }
  }
  for (uint32_t F = 0; F < N.Functions.size(); ++F) {
    step();
    const auto &Function = N.Functions[F];
    if (Function.Node >= Size || Function.Body >= Size ||
        (Function.ParentFunction != None && Function.ParentFunction >= F))
      throw Error("invalid_interface_relation_navigation");
    const auto &Node = S.Nodes[Function.Node];
    if ((Node.Kind != "FunctionDeclaration" &&
         Node.Kind != "FunctionExpression" &&
         Node.Kind != "ArrowFunctionExpression") ||
        Function.ID != identity("source-function", {N.ID, Node.ID}) ||
        child(Function.Node, "body") != Function.Body)
      throw Error("invalid_interface_relation_navigation");
  }
  for (uint32_t C = 0; C < N.Calls.size(); ++C) {
    step();
    const auto &Call = N.Calls[C];
    if (Call.Node >= Size || Calls[Call.Node] != None ||
        (Call.EnclosingFunction != None &&
         Call.EnclosingFunction >= N.Functions.size()) ||
        Call.ID != identity("source-call", {N.ID, S.Nodes[Call.Node].ID}))
      throw Error("invalid_interface_relation_navigation");
    Calls[Call.Node] = C;
  }
  for (uint32_t R = 0; R < I.Records.size(); ++R) {
    step();
    const auto &Request = I.Records[R];
    if (Request.Node >= Size ||
        Request.ID != identity("source-interface",
                               {I.ID, S.Nodes[Request.Node].ID, Request.Kind}))
      throw Error("invalid_interface_relation_request");
    if (Request.Kind != "fetch" && Request.Kind != "fetch_member_call")
      continue;
    if (Requests[Request.Node] != None || Calls[Request.Node] == None ||
        (S.Nodes[Request.Node].Kind != "CallExpression" &&
         S.Nodes[Request.Node].Kind != "OptionalCallExpression"))
      throw Error("invalid_interface_relation_request");
    Requests[Request.Node] = R;
  }
  for (uint32_t R = 0; R < B.References.size(); ++R) {
    step();
    const auto &Reference = B.References[R];
    if (Reference.Node >= Size || References[Reference.Node] != None ||
        S.Nodes[Reference.Node].Kind != "Identifier" ||
        (Reference.Binding != None && Reference.Binding >= B.Bindings.size()))
      throw Error("invalid_interface_relation_binding");
    References[Reference.Node] = R;
    if (Reference.Binding != None &&
        (Reference.Access == "write" || Reference.Access == "read_write"))
      WrittenBindings[Reference.Binding] = true;
  }
  for (const auto &Scope : B.Scopes) {
    step();
    if (Scope.PossibleDirectEval)
      throw Error("interface_relation_dynamic_eval");
  }
}

const std::u16string *Context::member(uint32_t Node) {
  if (Node == None)
    return nullptr;
  const auto &M = S.Nodes[Node];
  if ((M.Kind != "MemberExpression" && M.Kind != "OptionalMemberExpression") ||
      M.flag("computed"))
    return nullptr;
  const auto Property = child(Node, "property");
  return Property != None && S.Nodes[Property].Kind == "Identifier"
             ? S.Nodes[Property].text("name")
             : nullptr;
}

uint32_t Context::binding(uint32_t Node) const {
  if (Node == None || References[Node] == None)
    return None;
  const auto &R = B.References[References[Node]];
  return R.Resolution == "lexical_binding" ? R.Binding : None;
}

uint32_t Context::function(uint32_t Node) const {
  if (Node == None || Calls[Node] == None)
    return None;
  const auto F = N.Calls[Calls[Node]].EnclosingFunction;
  return F == None ? None : N.Functions[F].Node;
}

bool Context::spread(uint32_t Node) {
  for (const auto &C : S.Nodes[Node].Children) {
    step();
    if (C.Field == "arguments" && S.Nodes[C.Index].Kind == "SpreadElement")
      return true;
  }
  return false;
}

void Context::publish(InterfaceSourceRelation R) {
  step();
  if (A.Records.size() >= Limits.Records)
    throw Error("interface_relation_record_budget_exceeded");
  R.ID =
      identity("source-interface-relation", {A.ID, I.Records.at(R.Request).ID,
                                             R.Kind, S.Nodes.at(R.Node).ID});
  A.Records.push_back(std::move(R));
}

} // namespace neverd::web::interface_relations

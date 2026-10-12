//===- ResultLinks.cpp - Request result member-use syntax --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed member spellings and direct then-handler anchors, never API proofs.
///
//===----------------------------------------------------------------------===//

#include "RelationContext.h"

namespace neverd::web::interface_relations {
namespace {
std::string memberKind(std::u16string_view Name) {
  if (Name == u"json")
    return "json";
  if (Name == u"text")
    return "text";
  if (Name == u"arrayBuffer")
    return "array_buffer";
  if (Name == u"blob")
    return "blob";
  if (Name == u"formData")
    return "form_data";
  if (Name == u"bytes")
    return "bytes";
  if (Name == u"clone")
    return "clone";
  return {};
}
} // namespace

void resultLinks(Context &C, uint32_t Node) {
  const auto &Call = C.S.Nodes[Node];
  if (Call.Kind != "CallExpression" && Call.Kind != "OptionalCallExpression")
    return;
  const auto Callee = C.child(Node, "callee");
  const auto *Property = C.member(Callee);
  if (!Property)
    return;
  const auto Receiver = C.child(Callee, "object");
  if (Receiver == None)
    return;
  InterfaceSourceRelation R;
  R.Node = Node;
  R.ReceiverNode = Receiver;
  R.FunctionNode = C.function(Node);
  R.ArgumentsStatus = C.spread(Node) ? "spread_unresolved" : "positional";
  if (*Property == u"then" && C.Requests[Receiver] != None) {
    R.Request = C.Requests[Receiver];
    R.Kind = "then_callback_syntax";
    R.MemberKind = "then";
    if (R.ArgumentsStatus == "spread_unresolved")
      R.HandlerStatus = "spread_unresolved";
    else {
      R.HandlerNode = C.child(Node, "arguments");
      R.HandlerStatus = "absent";
      if (R.HandlerNode != None) {
        const auto &Handler = C.S.Nodes[R.HandlerNode];
        R.HandlerStatus = Handler.Kind == "FunctionExpression" ||
                                  Handler.Kind == "ArrowFunctionExpression"
                              ? "function_expression_syntax"
                          : Handler.Kind == "Identifier" ? "identifier_syntax"
                                                         : "value_syntax";
      }
    }
    C.publish(std::move(R));
    return;
  }
  R.MemberKind = memberKind(*Property);
  if (R.MemberKind.empty())
    return;
  if (C.S.Nodes[Receiver].Kind == "AwaitExpression") {
    const auto Argument = C.child(Receiver, "argument");
    if (Argument != None)
      R.Request = C.Requests[Argument];
    R.Kind = "awaited_result_member_call";
  } else {
    R.Binding = C.binding(Receiver);
    if (R.Binding != None)
      R.Request = C.ResultBindings[R.Binding];
    R.Kind = "bound_result_member_call";
  }
  if (R.Request != None)
    C.publish(std::move(R));
}

} // namespace neverd::web::interface_relations

//===- RequestLinks.cpp - Request containment and result bindings ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Direct return and awaited const-initializer syntax, without caller flow.
///
//===----------------------------------------------------------------------===//

#include "RelationContext.h"

#include "neverd/web/Error.h"

namespace neverd::web::interface_relations {

void requestLinks(Context &C, uint32_t Request) {
  const auto Node = C.I.Records[Request].Node;
  if (C.Requests[Node] == None)
    return;
  const auto Function = C.function(Node);
  if (Function != None) {
    InterfaceSourceRelation R;
    R.Request = Request;
    R.Kind = "request_function_containment";
    R.Node = Node;
    R.FunctionNode = Function;
    C.publish(std::move(R));
  }
  auto Result = Node;
  const auto Parent = C.Parents[Node];
  const bool Awaited = Parent != None &&
                       C.S.Nodes[Parent].Kind == "AwaitExpression" &&
                       C.child(Parent, "argument") == Node;
  if (Awaited)
    Result = Parent;
  const auto Owner = C.Parents[Result];
  if (Owner == None)
    return;
  const auto &O = C.S.Nodes[Owner];
  if ((O.Kind == "ReturnStatement" && C.child(Owner, "argument") == Result) ||
      (O.Kind == "ArrowFunctionExpression" &&
       C.child(Owner, "body") == Result)) {
    InterfaceSourceRelation R;
    R.Request = Request;
    R.Kind = "direct_request_return";
    R.Node = Owner;
    R.FunctionNode = Function;
    C.publish(std::move(R));
  }
  if (!Awaited || O.Kind != "VariableDeclarator" ||
      C.child(Owner, "init") != Result)
    return;
  const auto Declaration = C.Parents[Owner];
  const auto Name = C.child(Owner, "id");
  if (Declaration == None || Name == None ||
      C.S.Nodes[Declaration].Kind != "VariableDeclaration" ||
      !C.S.Nodes[Declaration].text("kind") ||
      *C.S.Nodes[Declaration].text("kind") != u"const" ||
      C.S.Nodes[Name].Kind != "Identifier")
    return;
  const auto Binding = C.Declarations[Name];
  if (Binding == None || C.WrittenBindings[Binding])
    return;
  if (C.ResultBindings[Binding] != None)
    throw Error("interface_relation_ambiguous_result_binding");
  // The const initializer establishes only a source value-origin candidate.
  // Object properties, response identity and actual execution remain unknown.
  C.ResultBindings[Binding] = Request;
  InterfaceSourceRelation R;
  R.Request = Request;
  R.Kind = "await_result_const_binding";
  R.Node = Owner;
  R.FunctionNode = Function;
  R.ReceiverNode = Name;
  R.Binding = Binding;
  C.publish(std::move(R));
}

} // namespace neverd::web::interface_relations

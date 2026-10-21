//===- TransformationLinks.cpp - Nearby request transform syntax -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed call spellings and direct syntax containment, never value dataflow.
///
//===----------------------------------------------------------------------===//

#include "RelationContext.h"

#include "neverd/web/Limits.h"

namespace neverd::web::interface_relations {
namespace {
std::u16string spelling(Context &C, uint32_t Node, uint32_t Depth = 0) {
  C.step();
  if (Node == None || Depth == 4)
    return {};
  const auto &N = C.S.Nodes[Node];
  if (N.Kind == "Identifier") {
    const auto *Name = N.text("name");
    return Name ? *Name : std::u16string{};
  }
  const auto *Member = C.member(Node);
  if (!Member)
    return {};
  auto Base = spelling(C, C.child(Node, "object"), Depth + 1);
  if (Base.empty())
    return {};
  return Base + u"." + *Member;
}

std::string transform(Context &C, uint32_t Node) {
  const auto Name = spelling(C, C.child(Node, "callee"));
  struct Rule {
    std::u16string_view Name;
    std::string_view Kind;
  };
  constexpr Rule Rules[] = {{u"JSON.stringify", "json_stringify"},
                            {u"JSON.parse", "json_parse"},
                            {u"btoa", "base64_encode"},
                            {u"atob", "base64_decode"},
                            {u"encodeURIComponent", "uri_component_encode"},
                            {u"decodeURIComponent", "uri_component_decode"},
                            {u"crypto.subtle.encrypt", "crypto_encrypt"},
                            {u"crypto.subtle.decrypt", "crypto_decrypt"},
                            {u"crypto.subtle.digest", "crypto_digest"},
                            {u"crypto.subtle.sign", "crypto_sign"},
                            {u"crypto.subtle.verify", "crypto_verify"},
                            {u"zlib.gzip", "gzip"},
                            {u"zlib.gunzip", "gunzip"},
                            {u"zlib.deflate", "deflate"},
                            {u"zlib.inflate", "inflate"}};
  for (const auto &R : Rules) {
    C.step();
    if (Name == R.Name)
      return std::string(R.Kind);
  }
  return {};
}

uint32_t resultRequest(Context &C, uint32_t Node) {
  const auto Callee = C.child(Node, "callee");
  const auto *Name = C.member(Callee);
  if (!Name || (*Name != u"json" && *Name != u"text" &&
                *Name != u"arrayBuffer" && *Name != u"bytes"))
    return None;
  const auto Receiver = C.child(Callee, "object");
  if (Receiver == None)
    return None;
  if (C.S.Nodes[Receiver].Kind == "AwaitExpression") {
    const auto Argument = C.child(Receiver, "argument");
    return Argument == None ? None : C.Requests[Argument];
  }
  const auto B = C.binding(Receiver);
  return B == None ? None : C.ResultBindings[B];
}
} // namespace

void transformationLinks(Context &C, uint32_t Node) {
  const auto &N = C.S.Nodes[Node];
  if (N.Kind != "CallExpression" && N.Kind != "OptionalCallExpression")
    return;
  auto Kind = transform(C, Node);
  if (Kind.empty())
    return;
  // The outer request body must be a directly admitted Fetch option. This is
  // containment only: argument position, execution and output remain unknown.
  for (uint32_t I = 0; I < C.I.Records.size(); ++I) {
    C.step();
    const auto &Request = C.I.Records[I];
    if (Request.BodyNode == None || Request.Kind != "fetch" ||
        Request.OptionsStatus != "explicit_data_properties")
      continue;
    auto At = Node;
    uint32_t Depth = 0;
    while (At != None &&
           Depth <= ::neverd::limits::web::kTransformationLinksDepthLimit) {
      C.step();
      const auto &AtNode = C.S.Nodes[At];
      if (At != Node && (AtNode.Kind == "FunctionExpression" ||
                         AtNode.Kind == "ArrowFunctionExpression" ||
                         AtNode.Kind == "FunctionDeclaration"))
        break;
      if (At == Request.BodyNode) {
        InterfaceSourceRelation R;
        R.Request = I;
        R.Node = Node;
        R.InputNode = At;
        R.FunctionNode = C.function(Node);
        R.Kind = "request_body_transform_syntax";
        R.MemberKind = Kind;
        R.ContainmentDepth = Depth;
        R.ArgumentsStatus = C.spread(Node) ? "spread_unresolved" : "positional";
        C.publish(std::move(R));
        break;
      }
      At = C.Parents[At];
      ++Depth;
    }
  }
  // A response member call must be a direct first argument, optionally
  // awaited. Do not follow aliases, callbacks or arbitrary nested expressions.
  if (C.spread(Node))
    return;
  auto Input = C.child(Node, "arguments");
  if (Input != None && C.S.Nodes[Input].Kind == "AwaitExpression")
    Input = C.child(Input, "argument");
  if (Input == None || C.S.Nodes[Input].Kind != "CallExpression")
    return;
  const auto Request = resultRequest(C, Input);
  if (Request == None)
    return;
  InterfaceSourceRelation R;
  R.Request = Request;
  R.Node = Node;
  R.InputNode = Input;
  R.FunctionNode = C.function(Node);
  R.Kind = "result_transform_argument_syntax";
  R.MemberKind = std::move(Kind);
  R.ArgumentsStatus = "positional";
  C.publish(std::move(R));
}
} // namespace neverd::web::interface_relations

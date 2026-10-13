//===- SourceFetch.cpp - Fetch-spelling and WebSocket syntax evidence ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Direct and member-spelling candidates, never runtime API claims.
///
//===----------------------------------------------------------------------===//

#include "SourceContext.h"

namespace neverd::web::interfaces {
void inspectFetchOptions(SourceContext &C, SourceInterface &R) {
  if (R.OptionsNode == None) {
    R.Method = "GET";
    R.MethodStatus = "omitted_init_default_candidate";
    return;
  }
  R.OptionsStatus = "unresolved";
  if (C.S.Nodes[R.OptionsNode].Kind != "ObjectExpression")
    return;
  std::map<std::u16string, uint32_t> Properties;
  for (const auto &Child : C.S.Nodes[R.OptionsNode].Children) {
    C.step();
    const auto &P = C.S.Nodes[Child.Index];
    if (Child.Field != "properties" || P.Kind != "Property" ||
        P.flag("computed") || P.flag("method") || !P.text("kind") ||
        *P.text("kind") != u"init")
      return;
    const auto Key = C.child(Child.Index, "key");
    if (Key == None)
      return;
    const auto &K = C.S.Nodes[Key];
    const auto *Name = K.text(K.Kind == "Identifier" ? "name" : "value");
    if (!Name || (K.Kind != "Identifier" && K.Kind != "StringLiteral") ||
        *Name == u"__proto__" ||
        !Properties.emplace(*Name, C.child(Child.Index, "value")).second)
      return;
  }
  R.OptionsStatus = "explicit_data_properties";
  auto Property = [&](std::u16string_view Name) {
    const auto Found = Properties.find(std::u16string(Name));
    return Found == Properties.end() ? None : Found->second;
  };
  R.MethodNode = Property(u"method");
  R.HeadersNode = Property(u"headers");
  R.BodyNode = Property(u"body");
  if (R.MethodNode == None) {
    // WebIDL dictionary conversion can observe inherited properties.
    R.MethodStatus = "inherited_method_unknown";
    return;
  }
  if (const auto *Text = C.constant(R.MethodNode)) {
    bool Forbidden = false;
    if (Text->size() > 16)
      R.Method = "other";
    else if (auto UTF8 = C.utf8(*Text)) {
      R.Method = interfaceMethod(*UTF8, true);
      Forbidden = interfaceForbiddenMethod(*UTF8);
    }
    R.MethodStatus = R.Method == "other" || R.Method == "unknown"
                         ? "unrecognized_redacted"
                         : "explicit_literal_candidate";
    if (Forbidden)
      R.MethodStatus = "fetch_forbidden_method";
  }
}
void inspectRequestSyntax(SourceContext &C, uint32_t I) {
  const auto &N = C.S.Nodes[I];
  if (N.Kind != "CallExpression" && N.Kind != "OptionalCallExpression" &&
      N.Kind != "NewExpression")
    return;
  const auto Callee = C.child(I, "callee");
  if (Callee == None)
    return;
  SourceInterface R;
  const auto *Property = C.member(Callee);
  if (N.Kind != "NewExpression" && Property && *Property == u"fetch") {
    // A property spelling is useful navigation evidence, not a platform API
    // or wrapper implementation. Do not apply Fetch option/method semantics.
    R.Kind = "fetch_member_call";
    R.ReceiverStatus = "member_spelling_syntax_only";
    R.ReceiverNode = C.child(Callee, "object");
    R.ReceiverBinding = C.binding(R.ReceiverNode);
    R.MethodStatus = "member_dispatch_unresolved";
    R.OptionsStatus = "wrapper_options_not_interpreted";
    R.Optional = C.S.Nodes[Callee].Kind == "OptionalMemberExpression" ||
                 C.S.Nodes[Callee].flag("optional");
  } else {
    if (C.S.Nodes[Callee].Kind != "Identifier")
      return;
    const auto *Name = C.S.Nodes[Callee].text("name");
    if (!Name || !((*Name == u"fetch" && N.Kind != "NewExpression") ||
                   (*Name == u"WebSocket" && N.Kind == "NewExpression")))
      return;
    if (!C.external(Callee, *Name)) {
      ++C.A.ExcludedDispatches;
      return;
    }
    R.Kind = *Name == u"fetch" ? "fetch" : "websocket";
  }
  R.Node = I;
  R.Optional =
      R.Optional || N.Kind == "OptionalCallExpression" || N.flag("optional");
  R.ArgumentsStatus = "positional";
  if (C.spreadArguments(I)) {
    R.ArgumentsStatus = "spread_unresolved";
    R.MethodStatus = "spread_unresolved";
    R.URLStatus = "spread_unresolved";
    R.Endpoint.Status = "spread_unresolved";
    R.OptionsStatus = "spread_unresolved";
    R.BodyStatus = "spread_unresolved";
    C.publish(std::move(R));
    return;
  }
  if (R.Kind == "fetch_member_call") {
    // The property spelling cannot establish even a URL argument role.
    R.URLStatus = "wrapper_arguments_not_interpreted";
    R.Endpoint.Status = "member_dispatch_unresolved";
    C.publish(std::move(R));
    return;
  }
  R.URLNode = C.child(I, "arguments");
  R.OptionsNode = C.child(I, "arguments", 1);
  if (R.Kind == "fetch")
    inspectFetchOptions(C, R);
  else if (R.Kind == "websocket")
    R.MethodStatus = "websocket_not_http_evidence";
  C.url(R);
  C.publish(std::move(R));
}

} // namespace neverd::web::interfaces

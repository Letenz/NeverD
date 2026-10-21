//===- SourceXHR.cpp - XMLHttpRequest lexical syntax evidence ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded constructor/receiver links, argument roles and event-value syntax.
/// These sites do not establish runtime identity, ordering or request sessions.
///
//===----------------------------------------------------------------------===//

#include "SourceContext.h"

#include "neverd/web/Limits.h"

namespace neverd::web::interfaces {
namespace {
bool constructor(SourceContext &C, uint32_t I) {
  return I != None && C.S.Nodes[I].Kind == "NewExpression" &&
         C.external(C.child(I, "callee"), u"XMLHttpRequest");
}

std::string eventKind(std::u16string_view Name) {
  // Fixed vocabulary only; arbitrary source text must never reach metadata.
  if (Name == u"readystatechange")
    return "readystatechange";
  if (Name == u"loadstart")
    return "loadstart";
  if (Name == u"progress")
    return "progress";
  if (Name == u"abort")
    return "abort";
  if (Name == u"error")
    return "error";
  if (Name == u"load")
    return "load";
  if (Name == u"timeout")
    return "timeout";
  if (Name == u"loadend")
    return "loadend";
  return {};
}

void open(SourceContext &C, SourceInterface &R) {
  R.MethodNode = C.child(R.Node, "arguments");
  R.URLNode = C.child(R.Node, "arguments", 1);
  R.AsyncNode = C.child(R.Node, "arguments", 2);
  R.CredentialArgumentsStatus = C.child(R.Node, "arguments", 3) != None ||
                                        C.child(R.Node, "arguments", 4) != None
                                    ? "present"
                                    : "absent";
  R.MethodStatus = R.MethodNode == None ? "missing_required_argument"
                                        : "dynamic_or_unavailable";
  if (const auto *Text = C.constant(R.MethodNode)) {
    bool Forbidden = false;
    if (Text->size() > 16)
      R.Method = "other";
    else if (const auto UTF8 = C.utf8(*Text)) {
      R.Method = interfaceMethod(*UTF8, true);
      Forbidden = interfaceForbiddenMethod(*UTF8);
    }
    R.MethodStatus = R.Method == "other" || R.Method == "unknown"
                         ? "unrecognized_redacted"
                         : "explicit_literal_candidate";
    if (Forbidden)
      R.MethodStatus = "xhr_forbidden_method";
  }
  if (R.URLNode == None) {
    R.URLStatus = "missing_required_argument";
    R.Endpoint.Status = "missing_required_argument";
  } else {
    C.url(R);
  }
}

void header(SourceContext &C, SourceInterface &R) {
  R.HeaderNameNode = C.child(R.Node, "arguments");
  R.HeaderValueNode = C.child(R.Node, "arguments", 1);
  R.HeaderKind = R.HeaderNameNode == None ? "absent" : "dynamic_or_unavailable";
  if (const auto *Text = C.constant(R.HeaderNameNode)) {
    // Recognized field names are short ASCII strings. A larger name cannot
    // enter the allowlist and need not be copied or published.
    if (Text->size() > ::neverd::limits::web::kSourceXHRHeaderTextSizeLimit)
      R.HeaderKind = "other";
    else if (const auto UTF8 = C.utf8(*Text))
      R.HeaderKind = interfaceHeaderKind(*UTF8);
    else
      R.HeaderKind = "unsupported_utf16";
  }
}

void handler(SourceContext &C, SourceInterface &R) {
  if (R.HandlerNode == None) {
    R.HandlerStatus = "absent";
    return;
  }
  const auto &Kind = C.S.Nodes[R.HandlerNode].Kind;
  if (Kind == "FunctionExpression" || Kind == "ArrowFunctionExpression")
    R.HandlerStatus = "function_syntax";
  else if (Kind == "NullLiteral")
    R.HandlerStatus = "null_value_syntax";
  else if (Kind == "ObjectExpression")
    R.HandlerStatus = "object_value_syntax";
  else
    R.HandlerStatus = "value_syntax";
}
} // namespace

std::vector<uint32_t> xhrConstructors(SourceContext &C) {
  std::vector<uint32_t> Constructors(C.B.Bindings.size(), None);
  for (uint32_t I = 0; I < C.S.Nodes.size(); ++I) {
    C.step();
    if (C.S.Nodes[I].Kind != "VariableDeclarator")
      continue;
    const auto Id = C.child(I, "id");
    const auto Init = C.child(I, "init");
    const auto Parent = C.Parents[I];
    if (Id == None || C.S.Nodes[Id].Kind != "Identifier" || Parent == None ||
        C.S.Nodes[Parent].Kind != "VariableDeclaration")
      continue;
    const auto *Kind = C.S.Nodes[Parent].text("kind");
    const auto B = C.Declarations[Id];
    if (!Kind || *Kind != u"const" || B >= C.B.Bindings.size())
      continue;
    const auto &Binding = C.B.Bindings[B];
    if (Binding.Kind != "const" || !Binding.Immutable || Binding.Conflicting ||
        C.WrittenBindings[B] || !constructor(C, Init))
      continue;
    Constructors[B] = Init;
  }
  return Constructors;
}

void inspectXHR(SourceContext &C, const std::vector<uint32_t> &Constructors,
                uint32_t I) {
  const auto &N = C.S.Nodes[I];
  const bool Assignment = N.Kind == "AssignmentExpression";
  if (!Assignment && N.Kind != "CallExpression" &&
      N.Kind != "OptionalCallExpression")
    return;
  const auto Member = C.child(I, Assignment ? "left" : "callee");
  const auto *Name = C.member(Member);
  if (!Name)
    return;
  SourceInterface R;
  R.Node = I;
  if (Assignment) {
    const auto *Operator = N.text("operator");
    if (!Operator || *Operator != u"=" || Name->size() < 3 ||
        Name->substr(0, 2) != u"on")
      return;
    R.EventKind = eventKind(std::u16string_view(*Name).substr(2));
    if (R.EventKind.empty())
      return;
    R.Kind = "xhr_event_handler";
  } else if (*Name == u"open") {
    R.Kind = "xhr_open";
  } else if (*Name == u"setRequestHeader") {
    R.Kind = "xhr_set_request_header";
  } else if (*Name == u"send") {
    R.Kind = "xhr_send";
  } else if (*Name == u"addEventListener") {
    R.Kind = "xhr_event_handler";
  } else {
    return;
  }
  const auto Receiver = C.child(Member, "object");
  R.ReceiverNode = Receiver;
  if (constructor(C, Receiver)) {
    R.ConstructorNode = Receiver;
    R.ReceiverStatus = "direct_constructor_syntax";
  } else {
    const auto B = C.binding(Receiver);
    if (B == None || Constructors[B] == None) {
      ++C.A.ExcludedDispatches;
      return;
    }
    R.ReceiverBinding = B;
    R.ConstructorNode = Constructors[B];
    R.ReceiverStatus = "const_binding_syntax";
  }
  R.Optional = N.Kind == "OptionalCallExpression" || N.flag("optional") ||
               C.S.Nodes[Member].Kind == "OptionalMemberExpression" ||
               C.S.Nodes[Member].flag("optional");
  R.ArgumentsStatus = Assignment ? "not_applicable" : "positional";
  if (Assignment) {
    R.HandlerNode = C.child(I, "right");
    handler(C, R);
  } else if (C.spreadArguments(I)) {
    // Ordinal positions in an AST with spread are not argument positions.
    // Do not make even absent/default claims about individual parameters.
    R.ArgumentsStatus = "spread_unresolved";
    if (R.Kind == "xhr_open") {
      R.MethodStatus = R.URLStatus = R.CredentialArgumentsStatus =
          R.Endpoint.Status = "spread_unresolved";
    } else if (R.Kind == "xhr_send") {
      R.BodyStatus = "spread_unresolved";
    } else if (R.Kind == "xhr_set_request_header") {
      R.HeaderKind = "spread_unresolved";
    } else {
      R.EventKind = R.HandlerStatus = R.OptionsStatus = "spread_unresolved";
    }
  } else if (R.Kind == "xhr_open") {
    open(C, R);
  } else if (R.Kind == "xhr_set_request_header") {
    header(C, R);
  } else if (R.Kind == "xhr_send") {
    R.BodyNode = C.child(I, "arguments");
    R.BodyStatus = R.BodyNode == None ? "absent_argument" : "argument_syntax";
  } else {
    const auto *Event = C.constant(C.child(I, "arguments"));
    if (!Event || (R.EventKind = eventKind(*Event)).empty())
      return;
    R.HandlerNode = C.child(I, "arguments", 1);
    R.OptionsNode = C.child(I, "arguments", 2);
    R.OptionsStatus = R.OptionsNode == None ? "absent" : "argument_syntax";
    handler(C, R);
  }
  C.publish(std::move(R));
}

} // namespace neverd::web::interfaces

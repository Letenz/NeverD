//===- SourceWrappers.cpp - Lexical fetch parameter forwarding ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded single-return wrapper syntax, without runtime dispatch claims.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "SourceContext.h"

#include <array>

namespace neverd::web::interfaces {
namespace {
uint32_t wrapperBinding(SourceContext &C, uint32_t Function,
                        const std::vector<uint32_t> &Declarations) {
  if (C.S.Nodes[Function].Kind == "FunctionDeclaration") {
    const auto Name = C.child(Function, "id");
    return Name == None ? None : Declarations[Name];
  }
  const auto Declarator = C.Parents[Function];
  if (Declarator == None ||
      C.S.Nodes[Declarator].Kind != "VariableDeclarator" ||
      C.child(Declarator, "init") != Function)
    return None;
  const auto Owner = C.Parents[Declarator];
  const auto Name = C.child(Declarator, "id");
  if (Owner == None || Name == None ||
      C.S.Nodes[Owner].Kind != "VariableDeclaration" ||
      !C.S.Nodes[Owner].text("kind") ||
      *C.S.Nodes[Owner].text("kind") != u"const" ||
      C.S.Nodes[Name].Kind != "Identifier")
    return None;
  return Declarations[Name];
}

uint32_t returnedRequest(SourceContext &C, uint32_t Function) {
  auto Body = C.child(Function, "body");
  if (Body == None)
    return None;
  if (C.S.Nodes[Body].Kind == "BlockStatement") {
    const auto &Statements = C.S.Nodes[Body].Children;
    if (Statements.size() != 1 || Statements[0].Field != "body" ||
        C.S.Nodes[Statements[0].Index].Kind != "ReturnStatement")
      return None;
    Body = C.child(Statements[0].Index, "argument");
  }
  if (Body == None || C.S.Nodes[Body].Kind != "CallExpression" ||
      C.S.Nodes[Body].flag("optional") ||
      !C.external(C.child(Body, "callee"), u"fetch") || C.spreadArguments(Body))
    return None;
  return Body;
}

std::optional<RequestWrapper>
forwarding(SourceContext &C, uint32_t Function,
           const std::vector<uint32_t> &Declarations) {
  const auto &N = C.S.Nodes[Function];
  if (N.flag("async") || N.flag("generator"))
    return {};
  const auto Request = returnedRequest(C, Function);
  if (Request == None)
    return {};
  std::array<uint32_t, 2> Parameters{None, None};
  std::array<uint32_t, 2> Bindings{None, None};
  uint32_t Count = 0;
  for (const auto &Child : N.Children) {
    C.step();
    if (Child.Field != "params")
      continue;
    if (Count == Parameters.size() || Child.Ordinal != Count ||
        C.S.Nodes[Child.Index].Kind != "Identifier")
      return {};
    const auto Binding = Declarations[Child.Index];
    if (Binding == None || C.B.Bindings[Binding].Kind != "parameter" ||
        C.WrittenBindings[Binding] || (Count && Bindings[0] == Binding))
      return {};
    Parameters[Count] = Child.Index;
    Bindings[Count++] = Binding;
  }
  if (!Count)
    return {};
  RequestWrapper R;
  R.Function = Function;
  R.Request = Request;
  uint32_t Arguments = 0;
  for (const auto &Child : C.S.Nodes[Request].Children) {
    C.step();
    if (Child.Field != "arguments")
      continue;
    if (Arguments == 2 || Child.Ordinal != Arguments)
      return {};
    const auto Binding = C.binding(Child.Index);
    uint32_t Parameter = 0;
    while (Parameter < Count && Bindings[Parameter] != Binding)
      ++Parameter;
    if (Binding == None || Parameter == Count)
      return {};
    if (!Arguments) {
      R.URLParameter = Parameters[Parameter];
      R.URLArgument = Parameter;
    } else {
      R.OptionsParameter = Parameters[Parameter];
      R.OptionsArgument = Parameter;
    }
    ++Arguments;
  }
  return Arguments ? std::optional<RequestWrapper>(R) : std::nullopt;
}
} // namespace

std::vector<RequestWrapper> requestWrappers(SourceContext &C) {
  auto Index = indexSourceDeclarations(C.S, C.B);
  C.step(Index.Steps);
  std::vector<uint32_t> Counts(C.B.Bindings.size());
  for (const auto Binding : Index.UniqueBindings) {
    C.step();
    if (Binding != None)
      ++Counts[Binding];
  }
  std::vector<RequestWrapper> Wrappers(C.B.Bindings.size());
  for (uint32_t I = 0; I < C.S.Nodes.size(); ++I) {
    C.step();
    const auto &Kind = C.S.Nodes[I].Kind;
    if (Kind != "FunctionDeclaration" && Kind != "FunctionExpression" &&
        Kind != "ArrowFunctionExpression")
      continue;
    const auto Binding = wrapperBinding(C, I, Index.UniqueBindings);
    if (Binding == None || Counts[Binding] != 1 || C.WrittenBindings[Binding])
      continue;
    if (auto R = forwarding(C, I, Index.UniqueBindings)) {
      Wrappers[Binding] = *R;
      ++C.A.WrapperDefinitions;
    }
  }
  return Wrappers;
}

void inspectRequestWrapper(SourceContext &C,
                           const std::vector<RequestWrapper> &Wrappers,
                           uint32_t I) {
  const auto &N = C.S.Nodes[I];
  if (N.Kind != "CallExpression" && N.Kind != "OptionalCallExpression")
    return;
  const auto Binding = C.binding(C.child(I, "callee"));
  if (Binding == None || Wrappers[Binding].Function == None)
    return;
  const auto &W = Wrappers[Binding];
  SourceInterface R;
  R.Kind = "fetch_wrapper_call";
  R.Node = I;
  R.WrapperNode = W.Function;
  R.WrapperRequestNode = W.Request;
  R.WrapperBinding = Binding;
  R.URLParameterNode = W.URLParameter;
  R.OptionsParameterNode = W.OptionsParameter;
  R.WrapperStatus = "single_return_lexical_forwarding_candidate";
  R.Optional = N.Kind == "OptionalCallExpression" || N.flag("optional");
  if (C.spreadArguments(I)) {
    R.ArgumentsStatus = "spread_unresolved";
    R.MethodStatus = R.URLStatus = R.Endpoint.Status = R.OptionsStatus =
        R.BodyStatus = "spread_unresolved";
  } else {
    R.ArgumentsStatus = "positional_parameter_forwarding";
    R.URLNode = C.child(I, "arguments", W.URLArgument);
    if (R.URLNode != None)
      R.URLArgumentIndex = W.URLArgument;
    if (W.OptionsArgument != None)
      R.OptionsNode = C.child(I, "arguments", W.OptionsArgument);
    if (R.OptionsNode != None)
      R.OptionsArgumentIndex = W.OptionsArgument;
    if (W.OptionsArgument != None && R.OptionsNode == None)
      R.MethodStatus = R.OptionsStatus = "wrapper_argument_missing";
    else
      inspectFetchOptions(C, R);
    if (R.URLNode == None)
      R.URLStatus = R.Endpoint.Status = "wrapper_argument_missing";
    else
      C.url(R);
  }
  C.publish(std::move(R));
}

} // namespace neverd::web::interfaces

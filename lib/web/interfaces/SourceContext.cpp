//===- SourceContext.cpp - Bounded source evidence admission ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One validated source model, bounded indexes and private URL admission.
///
//===----------------------------------------------------------------------===//

#include "SourceContext.h"

#include "../SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ConvertUTF.h"

namespace neverd::web::interfaces {

void SourceContext::step(uint64_t N) {
  if (N > sourceWorkLimit(S, MaxInterfaceSteps) - A.Steps)
    throw Error("interface_work_budget_exceeded");
  A.Steps += N;
}

void SourceContext::prepare() {
  step(validateSourceModel(S));
  if (B.SourceID != S.ID || V.SourceID != S.ID ||
      V.Nodes.size() != S.Nodes.size() ||
      (B.Status != "ok" && B.Status != "partial") ||
      B.References.size() > sourceLimits(S).Nodes ||
      B.Bindings.size() > sourceLimits(S).Nodes ||
      B.Scopes.size() > sourceLimits(S).Nodes ||
      B.Declarations.size() > 2 * sourceLimits(S).Nodes)
    throw Error("interface_source_evidence_mismatch");
  for (const auto &Scope : B.Scopes) {
    step();
    if (Scope.PossibleDirectEval)
      throw Error("interface_source_dynamic_eval");
  }
  References.assign(S.Nodes.size(), None);
  Declarations.assign(S.Nodes.size(), None);
  Parents.assign(S.Nodes.size(), None);
  WrittenBindings.assign(B.Bindings.size(), false);
  for (uint32_t I = 0; I < S.Nodes.size(); ++I)
    for (const auto &Child : S.Nodes[I].Children) {
      step();
      Parents[Child.Index] = I;
    }
  for (const auto &D : B.Declarations) {
    step();
    if (D.Node == None)
      continue;
    if (D.Node >= S.Nodes.size() || D.Binding >= B.Bindings.size())
      throw Error("invalid_interface_binding_model");
    auto &Binding = Declarations[D.Node];
    // A declaration can represent two environments (for example Annex B).
    // Such a site cannot establish the single const receiver admitted here.
    Binding = Binding == None || Binding == D.Binding ? D.Binding : None - 1;
  }
  for (uint32_t I = 0; I < B.References.size(); ++I) {
    step();
    const auto &R = B.References[I];
    if (R.Node >= S.Nodes.size() || References[R.Node] != None ||
        S.Nodes[R.Node].Kind != "Identifier" ||
        (R.Binding != None && R.Binding >= B.Bindings.size()))
      throw Error("invalid_interface_binding_model");
    References[R.Node] = I;
    if (R.Access != "write" && R.Access != "read_write")
      continue;
    if (R.Binding != None) {
      WrittenBindings[R.Binding] = true;
      continue;
    }
    const auto *Name = S.Nodes[R.Node].text("name");
    if (Name && (*Name == u"fetch" || *Name == u"WebSocket" ||
                 *Name == u"XMLHttpRequest"))
      WrittenGlobals.insert(*Name);
  }
}

uint32_t SourceContext::child(uint32_t I, std::string_view Field,
                              uint32_t Ordinal) {
  if (I == None)
    return None;
  for (const auto &C : S.Nodes.at(I).Children) {
    step();
    if (C.Field == Field && C.Ordinal == Ordinal)
      return C.Index;
  }
  return None;
}

const std::u16string *SourceContext::constant(uint32_t I) const {
  if (I == None)
    return nullptr;
  const auto &Value = V.Nodes.at(I);
  return Value.Status == "constant" && Value.Value &&
                 Value.Value->Kind == PrimitiveKind::String
             ? &Value.Value->String
             : nullptr;
}

const std::u16string *SourceContext::member(uint32_t I) {
  if (I == None)
    return nullptr;
  const auto &N = S.Nodes[I];
  if ((N.Kind != "MemberExpression" && N.Kind != "OptionalMemberExpression") ||
      N.flag("computed"))
    return nullptr;
  const auto Property = child(I, "property");
  return Property != None && S.Nodes[Property].Kind == "Identifier"
             ? S.Nodes[Property].text("name")
             : nullptr;
}

std::optional<std::string> SourceContext::utf8(std::u16string_view Text) {
  if (Text.size() > MaxInterfaceURLBytes)
    throw Error("interface_url_budget_exceeded");
  step(Text.size() + 1);
  const std::vector<llvm::UTF16> Units(Text.begin(), Text.end());
  std::string R;
  if (!llvm::convertUTF16ToUTF8String(Units, R))
    return {};
  return R;
}

bool SourceContext::external(uint32_t I, std::u16string_view Name) const {
  if (I == None || S.Nodes[I].Kind != "Identifier" || References[I] == None)
    return false;
  const auto *Actual = S.Nodes[I].text("name");
  return Actual && *Actual == Name &&
         B.References[References[I]].Resolution == "external" &&
         !WrittenGlobals.count(*Actual);
}

uint32_t SourceContext::binding(uint32_t I) const {
  if (I == None || References[I] == None)
    return None;
  const auto &R = B.References[References[I]];
  return R.Resolution == "lexical_binding" ? R.Binding : None;
}

bool SourceContext::spreadArguments(uint32_t I) {
  for (const auto &C : S.Nodes[I].Children) {
    step();
    if (C.Field == "arguments" && S.Nodes[C.Index].Kind == "SpreadElement")
      return true;
  }
  return false;
}

void SourceContext::url(SourceInterface &R) {
  if (R.URLNode == None)
    return;
  R.URLStatus = "dynamic_or_unavailable";
  R.Endpoint.Status = "dynamic_or_unavailable";
  const auto &N = S.Nodes[R.URLNode];
  if (N.Kind == "TemplateLiteral") {
    for (const auto &C : N.Children) {
      step();
      R.TemplateHoles += C.Field == "expressions";
    }
    R.URLStatus = "unresolved_template";
    R.Endpoint.Status = "unresolved_template";
  }
  const auto *Text = constant(R.URLNode);
  if (!Text)
    return;
  const auto UTF8 = utf8(*Text);
  if (!UTF8) {
    R.URLStatus = "unsupported_utf16";
    return;
  }
  R.URLStatus = "constant_string";
  R.Endpoint = inspectInterfaceEndpoint(*UTF8);
  const auto Size = R.Endpoint.Origin.size() + R.Endpoint.Path.size();
  if (Size > MaxInterfacePrivateBytes - A.PrivateBytes)
    throw Error("interface_private_budget_exceeded");
  A.PrivateBytes += Size;
  if ((R.Kind == "fetch" || R.Kind == "fetch_wrapper_call") &&
      R.Endpoint.CredentialsPresent)
    R.Endpoint.Status = "fetch_credentials_rejected";
  if (R.Kind == "websocket" && R.Endpoint.FragmentPresent)
    R.Endpoint.Status = "websocket_fragment_rejected";
}

void SourceContext::publish(SourceInterface R) {
  if (A.Records.size() >= MaxInterfaceRecords)
    throw Error("interface_record_budget_exceeded");
  R.ID = identity("source-interface", {A.ID, S.Nodes.at(R.Node).ID, R.Kind});
  A.Records.push_back(std::move(R));
}

} // namespace neverd::web::interfaces

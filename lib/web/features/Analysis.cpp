//===- Analysis.cpp - Client source syntax navigation ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded source-model admission and lexical callback candidate links.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"
#include "Internal.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"

namespace neverd::web::client_features {

void Context::step(uint64_t Count) {
  if (Count > sourceWorkLimit(S, MaxJavaScriptClientFeatureSteps) - A.Steps)
    throw Error("source_feature_budget_exceeded");
  A.Steps += Count;
}

uint32_t Context::child(uint32_t Node, std::string_view Field) {
  if (Node == None)
    return None;
  for (const auto &C : S.Nodes.at(Node).Children) {
    step();
    if (C.Field == Field)
      return C.Index;
  }
  return None;
}

const std::u16string *Context::key(uint32_t Node, bool Computed) {
  step();
  if (Node == None)
    return nullptr;
  const auto &K = S.Nodes.at(Node);
  if (K.Kind == "StringLiteral")
    return K.text("value");
  if (!Computed && K.Kind == "Identifier")
    return K.text("name");
  return nullptr;
}

bool Context::ordinaryProperty(uint32_t Node) const {
  const auto &P = S.Nodes.at(Node);
  const auto *Kind = P.text("kind");
  return P.Kind == "Property" && !P.flag("method") && Kind && *Kind == u"init";
}

void Context::prepare() {
  step(validateSourceModel(S));
  if (N.SourceID != S.ID || N.BindingID != B.ID || B.SourceID != S.ID)
    throw Error("source_feature_navigation_mismatch");
  if (N.Status != "ok" && N.Status != "partial")
    throw Error("source_feature_navigation_unavailable");
  const auto Size = S.Nodes.size();
  Parents.assign(Size, None);
  Functions.assign(Size, None);
  FunctionNodes.assign(Size, None);
  Calls.assign(Size, None);
  References.assign(Size, None);
  if (N.Functions.size() > Size || N.Calls.size() > Size ||
      B.Bindings.size() > 2 * sourceLimits(S).Nodes ||
      B.References.size() > sourceLimits(S).Nodes)
    throw Error("source_feature_budget_exceeded");
  BindingFunctions.assign(B.Bindings.size(), None);
  std::vector<bool> Ambiguous(B.Bindings.size());
  for (uint32_t I = 0; I < N.Functions.size(); ++I) {
    step();
    const auto &F = N.Functions[I];
    if (F.Node >= Size || FunctionNodes[F.Node] != None ||
        (S.Nodes[F.Node].Kind != "FunctionDeclaration" &&
         S.Nodes[F.Node].Kind != "FunctionExpression" &&
         S.Nodes[F.Node].Kind != "ArrowFunctionExpression"))
      throw Error("invalid_source_navigation_model");
    FunctionNodes[F.Node] = I;
    for (const auto Binding : {F.NameBinding, F.InitializerBinding}) {
      step();
      if (Binding == None)
        continue;
      if (Binding >= B.Bindings.size())
        throw Error("invalid_source_navigation_model");
      auto &Previous = BindingFunctions[Binding];
      if (Previous != None && Previous != I)
        Ambiguous[Binding] = true;
      Previous = I;
    }
  }
  for (uint32_t I = 0; I < BindingFunctions.size(); ++I) {
    step();
    if (Ambiguous[I])
      BindingFunctions[I] = None;
  }
  for (uint32_t I = 0; I < N.Calls.size(); ++I) {
    step();
    const auto &Call = N.Calls[I];
    if (Call.Node >= Size || Calls[Call.Node] != None)
      throw Error("invalid_source_navigation_model");
    Calls[Call.Node] = I;
  }
  for (uint32_t I = 0; I < Size; ++I) {
    step();
    auto Function = Functions[I];
    if (FunctionNodes[I] != None)
      Function = FunctionNodes[I];
    for (const auto &Child : S.Nodes[I].Children) {
      step();
      Parents[Child.Index] = I;
      Functions[Child.Index] = Function;
    }
  }
  // The navigation owner has already admitted this exact binding model.
  if (B.Status == "ok" || B.Status == "partial")
    for (const auto &Link : N.References) {
      step();
      if (Link.Reference >= B.References.size())
        throw Error("invalid_source_navigation_model");
      const auto &R = B.References[Link.Reference];
      if (R.Node >= Size || References[R.Node] != None ||
          (R.Binding != None && R.Binding >= B.Bindings.size()))
        throw Error("invalid_source_binding_model");
      References[R.Node] = Link.Reference;
    }
}

void Context::callback(SourceFeature &R) {
  if (R.Callback == None)
    return;
  step();
  R.CallbackFunction = FunctionNodes.at(R.Callback);
  const auto Ref = References.at(R.Callback);
  if (Ref == None)
    return;
  const auto &BRef = B.References[Ref];
  if (BRef.Resolution != "lexical_binding" || BRef.Binding == None)
    return;
  R.CallbackBinding = BRef.Binding;
  R.CallbackFunction = BindingFunctions.at(BRef.Binding);
}

void Context::publish(SourceFeature R) {
  step();
  if (A.Records.size() >= MaxJavaScriptClientFeatureRecords)
    throw Error("source_feature_record_budget_exceeded");
  R.Function = Functions.at(R.Node);
  R.Call = Calls.at(R.Node);
  callback(R);
  const std::string_view Trigger =
      R.Trigger == None ? std::string_view{} : S.Nodes.at(R.Trigger).ID;
  R.ID = identity("source-feature", {A.ID, S.Nodes.at(R.Node).ID, Trigger,
                                     std::to_string(unsigned(R.Pattern))});
  A.Records.push_back(std::move(R));
}

} // namespace neverd::web::client_features

namespace neverd::web {

SourceFeatures analyzeClientSourceFeatures(const SourceAnalysis &S,
                                           const SourceBindingAnalysis &B,
                                           const SourceNavigation &N) {
  client_features::Context C{S, B, N};
  C.A.SourceID = S.ID;
  C.A.NavigationID = N.ID;
  C.A.ID = identity("client-source-features",
                    {S.ID, B.ID, N.ID, JavaScriptClientFeatureProfile});
  try {
    C.prepare();
    for (uint32_t I = 0; I < N.Calls.size(); ++I)
      client_features::recognizeCall(C, I);
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      C.step();
      if (S.Nodes[I].Kind == "ObjectExpression")
        client_features::recognizeObject(C, I);
      else if (S.Nodes[I].Kind == "Property")
        client_features::recognizeProperty(C, I);
    }
    C.A.Status = "partial";
  } catch (const Error &E) {
    C.A.Records.clear();
    C.A.ComputedMemberCalls = 0;
    const std::string_view Code(E.what());
    C.A.Status = Code == "source_feature_budget_exceeded" ||
                         Code == "source_feature_record_budget_exceeded"
                     ? "budget_exceeded"
                     : "unavailable";
    C.A.Diagnostics.push_back({std::string(Code), -1});
  }
  return std::move(C.A);
}

} // namespace neverd::web

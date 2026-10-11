//===- Source.cpp - Bounded Node source behavior inspection -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Consume shared source semantics without executing scripts or I/O APIs.
///
//===----------------------------------------------------------------------===//

#include "../../SourceModel.h"
#include "Rules.h"

#include "neverd/web/Error.h"
#include "neverd/web/NodeBehavior.h"
#include "neverd/web/SourceValues.h"

namespace neverd::web {
std::string node_behavior::analysisID(std::string_view SourceID) {
  return identity("node-behavior",
                  {SourceID, NodeBehaviorProfile, JavaScriptBindingProfile,
                   JavaScriptModuleProfile, JavaScriptOriginProfile,
                   JavaScriptValueProfile});
}

namespace {
constexpr auto None = NoSourceIndex;

struct Inspector {
  const SourceAnalysis &S;
  const SourceModuleAnalysis &M;
  const SourceOrigins &O;
  const SourceValueAnalysis &V;
  NodeBehavior &A;

  void step(uint64_t N = 1) { node_behavior::charge(A.Steps, N); }
  uint32_t child(uint32_t I, std::string_view Field) {
    uint32_t Result = None;
    for (const auto &C : S.Nodes.at(I).Children) {
      step();
      if (C.Field != Field)
        continue;
      if (Result != None)
        throw Error("invalid_node_behavior_source");
      Result = C.Index;
    }
    return Result;
  }
  const std::u16string *string(uint32_t Node) {
    if (Node == None || V.Nodes.size() != S.Nodes.size() ||
        (V.Status != "ok" && V.Status != "partial"))
      return nullptr;
    const auto &Value = V.Nodes[Node];
    if (Value.Status != "constant" || !Value.Value ||
        Value.Value->Kind != PrimitiveKind::String)
      return nullptr;
    return &Value.Value->String;
  }
  std::string encoding(const NodeBehaviorRecord &R,
                       const std::vector<uint32_t> &Arguments) {
    if (R.Spread || R.ArgumentCount != 2 || Arguments.size() != 2 ||
        !string(Arguments[0]))
      return "overload_unresolved";
    const auto *Text = string(Arguments[1]);
    if (!Text)
      return "encoding_unresolved";
    if (Text->size() > 9)
      return "outside_profile";
    std::string Lower;
    for (auto C : *Text) {
      step();
      if (C > 127)
        return "outside_profile";
      Lower.push_back(char(C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : C));
    }
    if (Lower == "base64" || Lower == "base64url" || Lower == "hex")
      return Lower;
    return "outside_profile";
  }
  void run() {
    step(validateSourceModel(S));
    if (M.SourceID != S.ID || O.SourceID != S.ID || V.SourceID != S.ID ||
        O.ModuleID != M.ID || O.BindingID != M.BindingID ||
        (M.Status != "ok" && M.Status != "partial"))
      throw Error("node_behavior_evidence_mismatch");
    if (O.Status != "partial" || O.Nodes.size() != S.Nodes.size())
      throw Error("node_behavior_origins_unavailable");
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      step();
      const auto &N = S.Nodes[I];
      if (N.Kind != "CallExpression" && N.Kind != "OptionalCallExpression" &&
          N.Kind != "NewExpression")
        continue;
      ++A.Calls;
      const auto Callee = child(I, "callee");
      if (Callee == None || !O.Nodes[Callee]) {
        ++A.UnattributedCalls;
        continue;
      }
      const auto &Origin = *O.Nodes[Callee];
      if (N.Kind == "NewExpression" || Origin.Construction != None) {
        ++A.ConstructionCalls;
        continue;
      }
      const auto Rule = node_behavior::match(Origin, M, A.Steps);
      if (Rule.API.empty()) {
        ++A.UnmatchedCalls;
        continue;
      }
      if (A.Records.size() >= MaxNodeBehaviorRecords)
        throw Error("node_behavior_record_budget_exceeded");
      NodeBehaviorRecord R;
      R.API = Rule.API;
      R.Kind = Rule.Kind;
      R.NodeID = N.ID;
      R.CalleeID = S.Nodes[Callee].ID;
      R.Start = N.Start;
      R.End = N.End;
      R.Optional = N.Kind == "OptionalCallExpression" || N.flag("optional");
      R.OriginEvidence = Origin.Evidence;
      const auto &Request = M.Requests[Origin.Request];
      if (Request.Node >= S.Nodes.size())
        throw Error("invalid_node_behavior_request");
      R.RequestID = Request.ID;
      R.RequestNodeID = S.Nodes[Request.Node].ID;
      if (Origin.Import != None)
        R.ImportID = M.Imports.at(Origin.Import).ID;
      if (S.Nodes[Callee].Kind == "MemberExpression" ||
          S.Nodes[Callee].Kind == "OptionalMemberExpression") {
        const auto Receiver = child(Callee, "object");
        if (Receiver != None)
          R.ReceiverID = S.Nodes[Receiver].ID;
      }
      std::vector<uint32_t> Arguments;
      for (const auto &C : N.Children) {
        step();
        if (C.Field != "arguments")
          continue;
        if (C.Ordinal != R.ArgumentCount++)
          throw Error("invalid_node_behavior_arguments");
        const bool Spread = S.Nodes[C.Index].Kind == "SpreadElement";
        R.Spread |= Spread;
        if (R.Arguments.size() < MaxNodeBehaviorArguments) {
          R.Arguments.push_back({S.Nodes[C.Index].ID, C.Ordinal, Spread});
          Arguments.push_back(C.Index);
        }
      }
      if (Rule.EncodedData) {
        R.Encoding = encoding(R, Arguments);
        if (R.Encoding == "base64" || R.Encoding == "base64url" ||
            R.Encoding == "hex")
          R.Kind = "encoded_data_conversion_call";
      }
      R.ID = identity("node-behavior-record", {A.ID, N.ID, R.API, R.Kind});
      A.Records.push_back(std::move(R));
    }
    A.Status = "partial";
    A.Reason = "source_call_syntax_not_runtime_behavior";
  }
};
} // namespace

NodeBehavior analyzeNodeBehavior(const SourceAnalysis &S) {
  NodeBehavior A;
  A.SourceID = S.ID;
  A.ArtifactID = S.ArtifactID;
  A.SourceHash = S.BlobHash;
  A.SourceType = S.SourceType;
  // The identity is stable even when upstream evidence is unavailable.
  A.ID = node_behavior::analysisID(S.ID);
  try {
    node_behavior::charge(A.Steps, validateSourceModel(S));
    const auto B = analyzeSourceBindings(S);
    const auto M = analyzeSourceModules(S, B);
    const auto O = analyzeSourceOrigins(S, B, M);
    const auto V = analyzeSourceValues(S);
    A.BindingID = B.ID;
    A.ModuleID = M.ID;
    A.OriginID = O.ID;
    A.ValueID = V.ID;
    A.OriginStatus = O.Status;
    A.OriginReason = O.Reason;
    A.ValueStatus = V.Status;
    Inspector{S, M, O, V, A}.run();
  } catch (const Error &E) {
    A.Records.clear();
    A.Calls = A.UnattributedCalls = A.UnmatchedCalls = A.ConstructionCalls = 0;
    A.Reason = E.what();
    A.Status = A.Reason.find("budget_exceeded") != std::string::npos
                   ? "budget_exceeded"
                   : "unavailable";
  }
  return A;
}

} // namespace neverd::web

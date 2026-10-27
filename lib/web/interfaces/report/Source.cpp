//===- Source.cpp - Passive source evidence projection ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Request, feature and literal message syntax with authoritative anchors.
///
//===----------------------------------------------------------------------===//

#include "../../SourceModel.h"
#include "SourceStructure.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/PassiveReport.h"
#include "neverd/web/ProtocolMethods.h"

#include <set>

namespace neverd::web {
namespace {
uint32_t child(const SourceAnalysis &S, uint32_t Node, std::string_view Field) {
  for (const auto &C : S.Nodes.at(Node).Children)
    if (C.Field == Field)
      return C.Index;
  return NoSourceIndex;
}
std::string method(const std::u16string *Name) {
  if (!Name || Name->size() > ::neverd::limits::web::kSourceMethodNameSizeLimit)
    return "other_redacted";
  std::string ASCII;
  for (char16_t C : *Name) {
    if (C > 127)
      return "other_redacted";
    ASCII.push_back(char(C));
  }
  return classifyMCPMethod(ASCII);
}
std::string domain(SourceFeatureDomain D) {
  switch (D) {
  case SourceFeatureDomain::Tool:
    return "tool";
  case SourceFeatureDomain::Configuration:
    return "configuration";
  case SourceFeatureDomain::Permission:
    return "permission";
  case SourceFeatureDomain::MCP:
    return "mcp";
  case SourceFeatureDomain::Hook:
    return "hook";
  }
  throw Error("invalid_source_feature_domain");
}
} // namespace

PassiveSourceEvidence
projectPassiveSource(const SourceAnalysis &S, const SourceBindingAnalysis &B,
                     const SourceNavigation &N, const SourceFeatures &F,
                     const SourceInterfaces &I,
                     const InterfaceSourceRelations &Relations) {
  if (B.SourceID != S.ID || N.SourceID != S.ID || N.BindingID != B.ID ||
      F.SourceID != S.ID || F.NavigationID != N.ID || I.SourceID != S.ID ||
      I.BindingID != B.ID || Relations.InterfaceID != I.ID ||
      Relations.NavigationID != N.ID)
    throw Error("passive_report_source_evidence_mismatch");
  PassiveSourceEvidence Result;
  Result.SourceID = S.ID;
  Result.ID =
      identity("passive-source-projection",
               {S.ID, N.ID, F.ID, I.ID, Relations.ID, PassiveReportProfile});
  Result.Steps = validateSourceModel(S);
  auto Step = [&] {
    if (Result.Steps >= MaxPassiveReportSteps)
      throw Error("passive_report_work_budget_exceeded");
    ++Result.Steps;
  };
  auto Publish = [&](PassiveReportRecord R, uint32_t Node) {
    Step();
    if (Result.Records.size() >= MaxPassiveReportRecords)
      throw Error("passive_report_record_budget_exceeded");
    const auto &At = S.Nodes.at(Node);
    R.SourceID = S.ID;
    R.ArtifactID = S.ArtifactID;
    R.EvidenceClass = "static_inference";
    R.Offset = At.Start;
    R.Length = At.End - At.Start;
    R.CoordinatesPresent = true;
    R.ID = identity("passive-source-record",
                    {Result.ID, R.Kind, R.AnchorID, R.RelatedID});
    Result.Records.push_back(std::move(R));
  };
  for (const auto &Request : I.Records) {
    Step();
    PassiveReportRecord R;
    R.Kind = "source_interface";
    R.AnchorID = Request.ID;
    R.RelatedID = S.Nodes.at(Request.Node).ID;
    R.Method = Request.Method;
    R.Reason = Request.URLStatus;
    Publish(std::move(R), Request.Node);
  }
  passive_report::projectRequestStructure(S, I, Relations, Result);
  for (const auto &Link : Relations.Records) {
    Step();
    PassiveReportRecord R;
    R.Kind = "source_request_relation";
    R.AnchorID = Link.ID;
    R.RelatedID = I.Records.at(Link.Request).ID;
    R.ParentID = S.Nodes.at(Link.Node).ID;
    R.Reason = Link.Kind;
    R.Domain = Link.MemberKind;
    Publish(std::move(R), Link.Node);
  }
  for (const auto &Feature : F.Records) {
    Step();
    PassiveReportRecord R;
    R.Kind = "source_client_feature";
    R.AnchorID = Feature.ID;
    R.RelatedID = S.Nodes.at(Feature.Node).ID;
    if (Feature.Function != NoSourceIndex)
      R.ParentID = N.Functions.at(Feature.Function).ID;
    R.Domain = domain(Feature.Domain);
    R.Reason = Feature.Status;
    if (Feature.Domain == SourceFeatureDomain::MCP &&
        Feature.Pattern == SourceFeaturePattern::MCPCall &&
        !Feature.SpreadArguments) {
      const auto &Trigger = S.Nodes.at(Feature.Trigger);
      const auto *Name =
          Trigger.text(Trigger.Kind == "Identifier" ? "name" : "value");
      if (Name && *Name == u"callTool")
        R.Method = "tools/call";
      else if (Name && *Name == u"listTools")
        R.Method = "tools/list";
    }
    Publish(std::move(R), Feature.Node);
  }
  // A direct literal message object is source syntax, even when passed to a
  // shadowed or unknown transport. No protocol/API identity is authenticated.
  for (const auto &Call : N.Calls) {
    Step();
    if (Call.Kind != "call" && Call.Kind != "optional_call")
      continue;
    for (const auto &Arg : S.Nodes.at(Call.Node).Children) {
      Step();
      if (Arg.Field != "arguments" ||
          S.Nodes.at(Arg.Index).Kind != "ObjectExpression")
        continue;
      uint32_t Version = NoSourceIndex, Method = NoSourceIndex;
      bool Ordinary = true;
      std::set<std::u16string> Keys;
      for (const auto &Property : S.Nodes.at(Arg.Index).Children) {
        Step();
        const auto &P = S.Nodes.at(Property.Index);
        const auto K = child(S, Property.Index, "key");
        const auto *Name =
            K == NoSourceIndex
                ? nullptr
                : S.Nodes[K].text(S.Nodes[K].Kind == "Identifier" ? "name"
                                                                  : "value");
        const auto *PropertyKind = P.text("kind");
        if (P.Kind != "Property" || P.flag("computed") || P.flag("method") ||
            !PropertyKind || *PropertyKind != u"init" || !Name ||
            *Name == u"__proto__" || !Keys.insert(*Name).second) {
          Ordinary = false;
          continue;
        }
        if (*Name == u"jsonrpc")
          Version = child(S, Property.Index, "value");
        else if (*Name == u"method")
          Method = child(S, Property.Index, "value");
      }
      if (Version == NoSourceIndex || Method == NoSourceIndex)
        continue;
      PassiveReportRecord R;
      R.Kind = "source_message_object";
      R.AnchorID = S.Nodes[Arg.Index].ID;
      R.RelatedID = Call.ID;
      if (Call.EnclosingFunction != NoSourceIndex)
        R.ParentID = N.Functions.at(Call.EnclosingFunction).ID;
      R.Domain = "mcp";
      R.Reason = "unresolved_message_shape";
      if (Ordinary && S.Nodes[Version].Kind == "StringLiteral" &&
          S.Nodes[Version].text("value") &&
          *S.Nodes[Version].text("value") == u"2.0" &&
          S.Nodes[Method].Kind == "StringLiteral") {
        R.Method = method(S.Nodes[Method].text("value"));
        R.Reason = "literal_jsonrpc_method_syntax";
      }
      Publish(std::move(R), Arg.Index);
    }
  }
  if (I.Status != "partial" || Relations.Status != "partial" ||
      F.Status != "partial")
    Result.Status = "coverage_gap";
  return Result;
}
} // namespace neverd::web

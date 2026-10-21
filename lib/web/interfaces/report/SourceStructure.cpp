//===- SourceStructure.cpp - Request header and body syntax trees --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Consume admitted argument roles without evaluating properties or calls.
///
//===----------------------------------------------------------------------===//

#include "SourceStructure.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <set>

namespace neverd::web::passive_report {
void RequestStructure::step(uint64_t Count) {
  if (Count > MaxPassiveReportSteps - Result.Steps)
    throw Error("passive_report_work_budget_exceeded");
  Result.Steps += Count;
}

uint32_t RequestStructure::child(uint32_t Node, std::string_view Field,
                                 uint32_t Ordinal) {
  for (const auto &C : Source.Nodes.at(Node).Children) {
    step();
    if (C.Field == Field && C.Ordinal == Ordinal)
      return C.Index;
  }
  return NoSourceIndex;
}

PassiveReportRecord RequestStructure::record(uint32_t Node, std::string Domain,
                                             std::string Edge,
                                             std::string Parent,
                                             uint64_t Ordinal) {
  step();
  const auto &N = Source.Nodes.at(Node);
  PassiveReportRecord R;
  R.Kind = "source_request_structure";
  R.EvidenceClass = "static_inference";
  R.AnchorID = Request.ID;
  R.RelatedID = N.ID;
  R.SourceID = Source.ID;
  R.ArtifactID = Source.ArtifactID;
  R.Domain = std::move(Domain);
  R.ShapeStatus = "syntax_only";
  R.Reason = "runtime_value_unknown";
  R.Offset = N.Start;
  R.Length = N.End - N.Start;
  R.CoordinatesPresent = true;
  R.Structure.emplace();
  auto &S = *R.Structure;
  S.GroupID = Request.ID;
  S.Edge = std::move(Edge);
  S.ParentNodeID = std::move(Parent);
  S.Ordinal = Ordinal;
  S.NodeID = identity("passive-source-structure",
                      {Result.ID, Request.ID, N.ID, R.Domain, S.Edge,
                       S.ParentNodeID, std::to_string(Ordinal)});
  R.ID = identity("passive-source-record", {Result.ID, S.NodeID});
  return R;
}

std::string RequestStructure::publish(PassiveReportRecord R) {
  if (Result.Records.size() >= MaxPassiveReportRecords)
    throw Error("passive_report_record_budget_exceeded");
  const auto ID = R.Structure->NodeID;
  Result.Records.push_back(std::move(R));
  return ID;
}

std::string RequestStructure::value(uint32_t Node, std::string Domain,
                                    std::string Edge, std::string Parent,
                                    uint64_t Ordinal, uint32_t Depth,
                                    std::string NameClass) {
  auto R = record(Node, Domain, std::move(Edge), std::move(Parent), Ordinal);
  R.Structure->NameClass = std::move(NameClass);
  const auto &N = Source.Nodes[Node];
  if (Depth == ::neverd::limits::web::kMaxRequestStructureDepth) {
    R.ShapeStatus = "syntax_depth_limit";
    return publish(std::move(R));
  }
  if (N.Kind == "ObjectExpression" || N.Kind == "ArrayExpression") {
    R.ShapeKind = N.Kind == "ObjectExpression" ? "object" : "array";
    R.Reason = "container_syntax_not_serialized_value";
  } else if (N.Kind == "StringLiteral" || N.Kind == "NumericLiteral" ||
             N.Kind == "BooleanLiteral" || N.Kind == "NullLiteral") {
    R.ShapeKind = N.Kind == "StringLiteral"    ? "string"
                  : N.Kind == "NumericLiteral" ? "number"
                  : N.Kind == "BooleanLiteral" ? "boolean"
                                               : "null";
    R.Reason = "literal_syntax";
  }
  const auto ID = publish(std::move(R));
  if (N.Kind == "ArrayExpression") {
    for (const auto &C : N.Children) {
      step();
      if (C.Field == "elements")
        value(C.Index, Domain, "array_element", ID, C.Ordinal, Depth + 1);
    }
  } else if (N.Kind == "ObjectExpression") {
    std::set<std::u16string> Keys;
    for (const auto &C : N.Children) {
      step();
      const auto &P = Source.Nodes[C.Index];
      const auto Key = child(C.Index, "key");
      const auto V = child(C.Index, "value");
      const auto *Name = Key == NoSourceIndex ? nullptr
                         : Source.Nodes[Key].Kind == "Identifier"
                             ? Source.Nodes[Key].text("name")
                         : Source.Nodes[Key].Kind == "StringLiteral"
                             ? Source.Nodes[Key].text("value")
                             : nullptr;
      if (Name)
        step(Name->size());
      if (C.Field != "properties" || P.Kind != "Property" ||
          P.flag("computed") || P.flag("method") || !P.text("kind") ||
          *P.text("kind") != u"init" || !Name || *Name == u"__proto__" ||
          V == NoSourceIndex || !Keys.insert(*Name).second) {
        auto Unknown =
            record(C.Index, Domain, "unresolved_property", ID, C.Ordinal);
        Unknown.Reason = "spread_accessor_computed_duplicate_or_special_key";
        publish(std::move(Unknown));
        continue;
      }
      std::string Classification = "withheld";
      if (Domain == "request_headers" && Depth == 0) {
        // Only the common fixed header vocabulary may escape the private AST.
        std::string ASCII;
        if (Name->size() <=
            ::neverd::limits::web::
                kSourceStructureRequestStructureValueArrayExpressionObjectExpressionNameSizeLimit)
          for (char16_t Unit : *Name)
            ASCII.push_back(Unit <= 127 ? char(Unit) : '?');
        Classification = interfaceHeaderKind(ASCII);
      }
      value(V, Domain, "object_field", ID, C.Ordinal, Depth + 1,
            std::move(Classification));
    }
  }
  return ID;
}

void projectRequestStructure(const SourceAnalysis &S, const SourceInterfaces &I,
                             const InterfaceSourceRelations &Relations,
                             PassiveSourceEvidence &Result) {
  for (uint32_t Index = 0; Index < I.Records.size(); ++Index) {
    const auto &Request = I.Records[Index];
    RequestStructure C{S, Request, Result};
    C.step();
    if (Request.URLNode != NoSourceIndex)
      C.url();
    if (Request.HeadersNode != NoSourceIndex)
      C.value(Request.HeadersNode, "request_headers", "root");
    if (Request.HeaderNameNode != NoSourceIndex) {
      // XHR header roles have already been admitted by the source owner.
      auto R = C.record(Request.HeaderNameNode, "request_headers",
                        "header_name", "", 0);
      R.Structure->NameClass = Request.HeaderKind;
      const auto Parent = C.publish(std::move(R));
      if (Request.HeaderValueNode != NoSourceIndex)
        C.value(Request.HeaderValueNode, "request_headers", "header_value",
                Parent);
    }
    if (Request.BodyNode == NoSourceIndex)
      continue;
    const auto Body = C.value(Request.BodyNode, "request_body", "root");
    for (const auto &Link : Relations.Records) {
      C.step();
      if (Link.Request != Index || Link.Node != Request.BodyNode ||
          Link.Kind != "request_body_transform_syntax" ||
          Link.MemberKind != "json_stringify" ||
          Link.ArgumentsStatus != "positional")
        continue;
      const auto Input = C.child(Link.Node, "arguments");
      if (Input != NoSourceIndex)
        C.value(Input, "json_stringify_input", "transform_input", Body);
      break;
    }
  }
}
} // namespace neverd::web::passive_report

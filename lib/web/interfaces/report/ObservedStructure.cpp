//===- ObservedStructure.cpp - Redacted observed JSON trees -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Field identity and observed presence without exposing keys or values.
///
//===----------------------------------------------------------------------===//

#include "ObservedStructure.h"

#include "neverd/web/Error.h"

namespace neverd::web::passive_report {
std::string jsonKind(const llvm::json::Value &V) {
  if (V.getAsObject())
    return "object";
  if (V.getAsArray())
    return "array";
  if (V.getAsString())
    return "string";
  if (V.getAsBoolean())
    return "boolean";
  if (V.getAsNumber())
    return "number";
  return "null";
}

void ObservedStructure::chargePrivate(uint64_t Bytes) {
  if (Bytes > MaxPassiveShapePrivateBytes - PrivateBytes)
    throw Error("passive_shape_private_budget_exceeded");
  PrivateBytes += Bytes;
}

uint32_t ObservedStructure::schemaNode(const PassiveReportRecord &Base,
                                       std::optional<uint32_t> Parent,
                                       std::string Edge, uint64_t Ordinal) {
  if (Records.size() + Schema.size() >= MaxPassiveReportRecords)
    throw Error("passive_report_record_budget_exceeded");
  const auto Index = uint32_t(Schema.size());
  SchemaNode N;
  N.Parent = Parent;
  N.Record = Base;
  N.Record.Kind = "observed_json_schema_node";
  N.Record.ShapeStatus = "observed_structure_aggregate";
  N.Record.ShapeKind = "type_union";
  N.Record.Types.clear();
  N.Record.Members = 0;
  N.Record.Reason = "observed_presence_not_server_optionality";
  auto &S = *N.Record.Structure;
  S.NodeID = identity("passive-shape-schema", {Capture.ID, PassiveReportProfile,
                                               std::to_string(Index)});
  S.SchemaNodeID = S.NodeID;
  S.ParentNodeID = Parent ? Schema[*Parent].Record.Structure->NodeID : "";
  S.Edge = std::move(Edge);
  S.Ordinal = Ordinal;
  S.Instances = 0;
  Schema.push_back(std::move(N));
  return Index;
}

void ObservedStructure::value(const llvm::json::Value &V,
                              PassiveReportRecord &Summary, uint32_t Index,
                              std::string Parent, uint64_t Ordinal) {
  if (Records.size() + Schema.size() >= MaxPassiveReportRecords)
    throw Error("passive_report_record_budget_exceeded");
  const auto Kind = jsonKind(V);
  ++Summary.Types[Kind];
  ++Schema[Index].Record.Types[Kind];
  ++*Schema[Index].Record.Structure->Instances;
  auto R = Summary;
  R.Kind = "observed_json_value_node";
  R.ShapeStatus = "observed_json_value";
  R.ShapeKind = Kind;
  R.Types = {{Kind, 1}};
  R.Members = V.getAsObject()  ? V.getAsObject()->size()
              : V.getAsArray() ? V.getAsArray()->size()
                               : 0;
  R.Reason = "recorded_json_structure";
  R.Structure = Schema[Index].Record.Structure;
  auto &S = *R.Structure;
  S.NodeID = identity("passive-shape-value",
                      {Capture.ID, PassiveReportProfile, Summary.AnchorID,
                       std::to_string(Records.size())});
  S.ParentNodeID = std::move(Parent);
  S.Ordinal = Ordinal;
  S.Instances.reset();
  const auto ParentID = S.NodeID;
  Records.push_back(std::move(R));
  if (const auto *Object = V.getAsObject()) {
    // JSON objects have no semantic order. Sort privately for reproducibility
    // and allocate opaque field ordinals on first observation at each parent.
    std::map<std::string_view, const llvm::json::Value *> Sorted;
    for (const auto &KV : *Object) {
      const llvm::StringRef Key = KV.first;
      Sorted.emplace(std::string_view(Key.data(), Key.size()), &KV.second);
    }
    uint64_t Position = 0;
    for (const auto &[Key, Child] : Sorted) {
      auto Found = Schema[Index].Fields.find(Key);
      uint32_t ChildIndex;
      if (Found == Schema[Index].Fields.end()) {
        chargePrivate(Key.size());
        ChildIndex = schemaNode(Summary, Index, "object_field",
                                Schema[Index].Fields.size());
        Schema[Index].Fields.emplace(std::string(Key), ChildIndex);
      } else
        ChildIndex = Found->second;
      value(*Child, Summary, ChildIndex, ParentID, Position++);
    }
  } else if (const auto *Array = V.getAsArray()) {
    uint64_t Position = 0;
    for (const auto &Child : *Array) {
      if (!Schema[Index].Element) {
        const auto ChildIndex = schemaNode(Summary, Index, "array_element", 0);
        Schema[Index].Element = ChildIndex;
      }
      value(Child, Summary, *Schema[Index].Element, ParentID, Position++);
    }
  }
}

void ObservedStructure::inspect(const llvm::json::Value &V,
                                const HARObservation &O, bool Response,
                                PassiveReportRecord &Summary) {
  const bool Comparable =
      O.MethodStatus == "capture_declared" && O.Endpoint.comparable();
  const GroupKey Key{O.Method, O.Endpoint.Origin, O.Endpoint.Path, Response};
  const auto Found = Comparable ? Groups.find(Key) : Groups.end();
  uint32_t Root;
  if (Found == Groups.end()) {
    Summary.Structure.emplace();
    Summary.Structure->GroupID =
        identity("passive-shape-group", {Capture.ID, PassiveReportProfile, O.ID,
                                         Response ? "response" : "request"});
    Root = schemaNode(Summary, {}, "root", 0);
    if (Comparable) {
      chargePrivate(O.Method.size() + O.Endpoint.Origin.size() +
                    O.Endpoint.Path.size());
      Groups.emplace(Key, Root);
    }
  } else {
    Root = Found->second;
    Summary.Structure.emplace();
    Summary.Structure->GroupID = Schema[Root].Record.Structure->GroupID;
  }
  Summary.Structure->SchemaNodeID = Schema[Root].Record.Structure->NodeID;
  value(V, Summary, Root, "", 0);
}

void ObservedStructure::finish() {
  if (Records.size() + Schema.size() > MaxPassiveReportRecords)
    throw Error("passive_report_record_budget_exceeded");
  for (auto &N : Schema) {
    auto &S = *N.Record.Structure;
    if (S.Edge == "object_field") {
      const auto &Parent = Schema.at(*N.Parent).Record;
      const auto Found = Parent.Types.find("object");
      S.EligibleParents = Found == Parent.Types.end() ? 0 : Found->second;
      S.MissingInstances = *S.EligibleParents - *S.Instances;
    }
  }
  for (auto &N : Schema)
    Records.push_back(std::move(N.Record));
}
} // namespace neverd::web::passive_report

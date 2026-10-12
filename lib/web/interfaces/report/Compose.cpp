//===- Compose.cpp - Selected passive report composition ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit selections preserve static, observed and candidate evidence.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/Error.h"
#include "neverd/web/PassiveReport.h"
#include "neverd/web/ProtocolMethods.h"

namespace neverd::web {
PassiveReport
composePassiveReport(const PassiveSourceEvidence *Source,
                     const SourceInterfaces *Interfaces, const HARCapture *HAR,
                     const std::vector<PassiveReportRecord> &Shapes,
                     const StreamCapture *Stream) {
  if (!Source && !HAR && !Stream)
    throw Error("passive_report_empty_selection");
  if (bool(Source) != bool(Interfaces) ||
      (Source && Source->SourceID != Interfaces->SourceID) ||
      (!HAR && !Shapes.empty()))
    throw Error("passive_report_selection_mismatch");
  PassiveReport Result;
  Result.SourceID = Source ? Source->SourceID : "";
  Result.SourceStatus = Source ? Source->Status : "not_selected";
  Result.HARID = HAR ? HAR->ID : "";
  Result.StreamID = Stream ? Stream->ID : "";
  Result.ID =
      identity("passive-report", {Source ? Source->ID : "", Result.HARID,
                                  Result.StreamID, PassiveReportProfile});
  auto Step = [&](uint64_t Count = 1) {
    if (Count > MaxPassiveReportSteps - Result.Steps)
      throw Error("passive_report_work_budget_exceeded");
    Result.Steps += Count;
  };
  auto Publish = [&](PassiveReportRecord R) {
    Step();
    if (Result.Records.size() >= MaxPassiveReportRecords)
      throw Error("passive_report_record_budget_exceeded");
    R.ID = identity("passive-report-record",
                    {Result.ID, R.Kind, R.AnchorID, R.RelatedID});
    Result.StaticRecords += R.EvidenceClass == "static_inference";
    Result.ObservedRecords += R.EvidenceClass == "imported_observation";
    Result.Records.push_back(std::move(R));
  };
  if (Source) {
    Step(Source->Steps);
    for (const auto &R : Source->Records)
      Publish(R);
  }
  if (HAR) {
    for (const auto &O : HAR->Observations) {
      PassiveReportRecord R;
      R.Kind = "http_observation";
      R.EvidenceClass = "imported_observation";
      R.AnchorID = O.ID;
      R.RelatedID = O.RequestID;
      R.ParentID = O.ResponseID;
      R.ArtifactID = HAR->ArtifactID;
      R.Method = O.Method;
      R.Reason = !O.RequestPresent || !O.ResponsePresent
                     ? "missing_request_or_response"
                 : O.WebSocketExtensionPresent ? "websocket_frames_unsupported"
                                               : "imported_http_metadata";
      Publish(std::move(R));
    }
    for (const auto &R : Shapes)
      Publish(R);
  }
  if (Source && HAR && Interfaces->Status == "partial") {
    const auto Links = correlateInterfaces(*Interfaces, *HAR);
    Step(Links.Steps);
    for (const auto &Pair : Links.Pairs) {
      PassiveReportRecord R;
      R.Kind = "http_source_observation_candidate";
      R.EvidenceClass = "candidate_correlation";
      R.AnchorID = Interfaces->Records.at(Pair.Source).ID;
      R.RelatedID = HAR->Observations.at(Pair.Observation).ID;
      R.ParentID = Pair.ID;
      R.SourceID = Source->SourceID;
      R.Reason = "method_origin_path_equality";
      Publish(std::move(R));
      ++Result.HTTPPairs;
    }
  }
  if (!Stream)
    return Result;
  std::map<std::string, std::vector<const StreamRecord *>> Methods;
  for (const auto &Record : Stream->Records) {
    PassiveReportRecord R;
    R.Kind = "stream_observation";
    R.EvidenceClass = "imported_observation";
    R.AnchorID = Record.ID;
    R.ArtifactID = Stream->ArtifactID;
    R.Offset = Record.Offset;
    R.Length = Record.Length;
    R.CoordinatesPresent = true;
    R.Method = Record.Method;
    R.Direction = Record.Direction;
    R.ShapeKind = Record.JSONKind;
    R.Reason = Record.RPC == "not_selected" ? Record.Kind : Record.RPC;
    if (Record.Peer)
      R.RelatedID = Stream->Records.at(*Record.Peer).ID;
    Publish(std::move(R));
    if (Stream->Profile == "mcp-2025-06-18-stdio-jsonl-v1" &&
        (Record.RPC == "request" || Record.RPC == "notification") &&
        classifyMCPMethod(Record.Method) == Record.Method)
      Methods[Record.Method].push_back(&Record);
  }
  if (Source)
    for (const auto &Record : Source->Records) {
      Step();
      const auto Found = Methods.find(Record.Method);
      if (Found == Methods.end() || Record.Domain != "mcp" ||
          (Record.Kind != "source_message_object" &&
           Record.Kind != "source_client_feature"))
        continue;
      for (const auto *Observed : Found->second) {
        PassiveReportRecord R;
        R.Kind = "mcp_same_method_candidate";
        R.EvidenceClass = "candidate_correlation";
        R.AnchorID = Record.AnchorID;
        R.RelatedID = Observed->ID;
        R.ParentID = Record.ParentID;
        R.SourceID = Source->SourceID;
        R.Method = Record.Method;
        R.Reason = "known_method_spelling_only";
        Publish(std::move(R));
        ++Result.MethodCandidates;
      }
    }
  return Result;
}
} // namespace neverd::web

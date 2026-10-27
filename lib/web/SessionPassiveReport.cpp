//===- SessionPassiveReport.cpp - Selected passive report session -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit committed selections, revision-bound caches and metadata pages.
///
//===----------------------------------------------------------------------===//

#include "JsonReader.h"
#include "SessionInternal.h"
#include "interfaces/report/Publication.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web {
std::string Session::createPassiveReport(std::string_view Revision,
                                         std::string_view SelectionJSON) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto V = parseBoundedJSON(SelectionJSON, {1024, 2, 16, 64});
  const auto *O = V.getAsObject();
  if (!O || O->getInteger("schema_version") != 1)
    throw Error("invalid_passive_report_selection");
  for (const auto &KV : *O)
    if (KV.first != "schema_version" && KV.first != "interface_analysis_id" &&
        KV.first != "har_capture_id" && KV.first != "stream_capture_id")
      throw Error("invalid_passive_report_selection");
  auto Selected = [&](llvm::StringRef Key) {
    const auto *Value = O->get(Key);
    if (!Value)
      return std::string{};
    const auto Text = Value->getAsString();
    if (!Text || Text->empty() ||
        Text->size() >
            ::neverd::limits::web::
                kSessionPassiveReportSessionCreatePassiveReportSelectedTextSizeLimit)
      throw Error("invalid_passive_report_selection");
    return Text->str();
  };
  const auto InterfaceID = Selected("interface_analysis_id");
  const auto HARID = Selected("har_capture_id");
  const auto StreamID = Selected("stream_capture_id");
  const auto SelectionID =
      identity("passive-selection", {InterfaceID, HARID, StreamID});
  for (const auto &[ID, R] : State->PassiveReports)
    if (ID == SelectionID)
      return json(passive_report::summary(R, State->Revision));
  if (State->PassiveReports.size() >= 2)
    throw Error("passive_report_cache_budget_exceeded");
  const HARCapture *HAR = nullptr;
  const StreamCapture *Stream = nullptr;
  std::vector<PassiveReportRecord> Shapes;
  if (!HARID.empty()) {
    const auto Found = State->HARCaptures.find(HARID);
    if (Found == State->HARCaptures.end())
      throw Error("har_capture_not_committed");
    HAR = &Found->second;
    const auto View = State->artifactView(HAR->ArtifactID);
    if (!View || View->BlobHash != HAR->BlobHash)
      throw Error("passive_report_capture_hash_mismatch");
    Shapes = inspectPassiveHARShapes(
        *HAR, View->Content.read(0, View->Content.size(), MaxHARBytes));
  }
  if (!StreamID.empty()) {
    const auto Found = State->StreamCaptures.find(StreamID);
    if (Found == State->StreamCaptures.end())
      throw Error("stream_capture_not_committed");
    Stream = &Found->second;
  }
  std::optional<PassiveSourceEvidence> Source;
  const SourceInterfaces *Interfaces = nullptr;
  if (!InterfaceID.empty()) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
    throw Error("capability_unavailable");
#else
    const auto Found = State->InterfaceSources.find(InterfaceID);
    if (Found == State->InterfaceSources.end())
      throw Error("unknown_interface_analysis");
    Interfaces = &Found->second;
    const auto &S = State->Sources.at(Interfaces->SourceID);
    const auto &B = State->Bindings.at(Interfaces->SourceID);
    const auto Existing = State->Navigation.find(S.ID);
    const auto LocalN = Existing == State->Navigation.end()
                            ? web::analyzeSourceNavigation(S, B)
                            : SourceNavigation{};
    const auto &N = Existing == State->Navigation.end()
                        ? LocalN
                        : Existing->second.Evidence;
    const auto LocalF = Existing == State->Navigation.end()
                            ? analyzeClientSourceFeatures(S, B, N)
                            : SourceFeatures{};
    const auto &F = Existing == State->Navigation.end()
                        ? LocalF
                        : Existing->second.Features;
    const auto Relations =
        analyzeInterfaceSourceRelations(S, B, *Interfaces, N);
    Source = projectPassiveSource(S, B, N, F, *Interfaces, Relations);
#endif
  }
  auto Report = composePassiveReport(Source ? &*Source : nullptr, Interfaces,
                                     HAR, Shapes, Stream);
  // Serialize before committing. Retained projections are bounded by both
  // records and canonical metadata bytes, independently of input body size.
  uint64_t Bytes = 0;
  for (const auto &R : Report.Records) {
    const auto Size = json(passive_report::record(R)).size();
    if (Size > ::neverd::limits::web::kMaxPassiveReportExportBytes - Bytes)
      throw Error("passive_report_metadata_budget_exceeded");
    Bytes += Size;
  }
  auto Reply = json(passive_report::summary(Report, State->Revision));
  State->PassiveReports.emplace(SelectionID, std::move(Report));
  return Reply;
}

std::string Session::passiveReportRecords(std::string_view Revision,
                                          std::string_view ReportID,
                                          uint64_t Offset,
                                          uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const PassiveReport *Report = nullptr;
  for (const auto &[ID, R] : State->PassiveReports)
    if (R.ID == ReportID)
      Report = &R;
  if (!Report)
    throw Error("unknown_passive_report");
  if (!Limit ||
      Limit > ::neverd::limits::web::kSessionPassiveReportRecordsLimit ||
      Offset > Report->Records.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Report->Records.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I)
    Items.emplace_back(passive_report::record(Report->Records[I]));
  auto R = passive_report::summary(*Report, State->Revision);
  R["items"] = std::move(Items);
  R["offset"] = Offset;
  R["page_complete"] = End == Report->Records.size();
  R["next_offset"] = End == Report->Records.size() ? llvm::json::Value(nullptr)
                                                   : llvm::json::Value(End);
  return json(std::move(R));
}
} // namespace neverd::web

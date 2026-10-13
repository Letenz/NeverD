//===- PassiveReport.h - Selected passive evidence report ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata-only source, HAR, stream and observed JSON shape projections.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/InterfaceRelations.h"
#include "neverd/web/SourceFeatures.h"
#include "neverd/web/Streams.h"

#include <map>

namespace neverd::web {
inline constexpr std::string_view PassiveReportProfile =
    "passive-evidence-shape-report-v1";
inline constexpr uint64_t MaxPassiveReportRecords = 16384;
inline constexpr uint64_t MaxPassiveReportSteps = 8000000;
inline constexpr uint64_t MaxPassiveShapeBytes = 65536;
inline constexpr uint64_t MaxPassiveShapeNodes = 200000;

/// All textual classifications are closed vocabulary. Opaque IDs and original
/// coordinates are the only input-derived publication. Body keys/values, URLs,
/// names, recorded IDs and raw source never belong in this model.
struct PassiveReportRecord {
  std::string ID, Kind, EvidenceClass, AnchorID, RelatedID, ParentID;
  std::string SourceID, ArtifactID, Method = "unknown", Domain;
  std::string ShapeStatus = "not_selected", ShapeKind = "unknown", Reason;
  std::string Direction = "not_applicable";
  uint64_t Offset = 0, Length = 0, Members = 0;
  bool CoordinatesPresent = false;
  std::map<std::string, uint64_t> Types;
};

struct PassiveSourceEvidence {
  std::string ID, SourceID;
  std::vector<PassiveReportRecord> Records;
  uint64_t Steps = 0;
  std::string Status = "partial";
};

/// Uses existing source, bindings, navigation, features and request models;
/// no independent parser, target resolution or runtime configuration analysis.
PassiveSourceEvidence projectPassiveSource(
    const SourceAnalysis &Source, const SourceBindingAnalysis &Bindings,
    const SourceNavigation &Navigation, const SourceFeatures &Features,
    const SourceInterfaces &Interfaces,
    const InterfaceSourceRelations &Relations);

/// Inspect only explicitly JSON, unencoded HAR body text after capture commit.
/// Shape summaries count types; arbitrary keys, values and examples stay
/// private. Invalid/absent/encoded/oversized bodies have located limitation
/// records.
std::vector<PassiveReportRecord>
inspectPassiveHARShapes(const HARCapture &Capture, std::string_view Bytes);

struct PassiveReport {
  std::string ID, SourceID, HARID, StreamID;
  std::vector<PassiveReportRecord> Records;
  uint64_t Steps = 0, StaticRecords = 0, ObservedRecords = 0;
  uint64_t HTTPPairs = 0, MethodCandidates = 0;
  std::string SourceStatus = "not_selected";
};

/// Null inputs are explicit absent selections. HTTP joins reuse the existing
/// method/origin/path owner. MCP same-method candidates do not reconstruct
/// recorded request/response relationships or imply source execution.
PassiveReport
composePassiveReport(const PassiveSourceEvidence *Source,
                     const SourceInterfaces *Interfaces, const HARCapture *HAR,
                     const std::vector<PassiveReportRecord> &Shapes,
                     const StreamCapture *Stream);
} // namespace neverd::web

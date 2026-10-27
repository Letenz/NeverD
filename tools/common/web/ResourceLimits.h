//===- ResourceLimits.h - Resource limits and defaults -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resource ceilings, work budgets and defaults owned by this component.
/// Binary layouts and instruction/ABI constants remain with their models.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_COMMON_WEB_RESOURCELIMITS_H
#define NEVERD_TOOLS_COMMON_WEB_RESOURCELIMITS_H

#include <cstddef>
#include <cstdint>

namespace neverd::limits::web_tool {

// tools/common/web/Backend.cpp
inline constexpr int kBackendResultCodeErrorCodeStringFieldMax = 128;
inline constexpr int kDefaultSessionPageRows = 128;
inline constexpr int kMaxSmallSessionPageRows = 128;
inline constexpr unsigned
    kBackendExecuteWebPackageAssessmentCreateMaximumSelected = 16U;
inline constexpr unsigned
    kBackendExecuteWebPackageAssessmentCreateMaximumDefault = 4U;
inline constexpr int kMaxSessionPageRows = 512;
inline constexpr int
    kBackendExecuteWebPackageBehaviorAnalyzeWebElectronEntriesAnalyzeWebElectronIpcAnalyzeSourceGetRefConstStdStringSizeLimit =
        64;
inline constexpr int
    kBackendExecuteWebAsarExtractUnpackedDirectoryIdStringFieldMax = 64;
inline constexpr int kBackendExecuteWebSourceAnchorViewViewIdStringFieldMax =
    64;
inline constexpr int
    kBackendExecuteWebSourceViewChunkWebSourceViewRecordsLimitByteLimitSizeFieldDefault =
        65536;
inline constexpr int
    kBackendExecuteWebSourceViewChunkWebSourceViewRecordsLimitByteLimitSizeFieldMax =
        65536;

// tools/common/web/Backend.cpp
inline constexpr int kMaxWebAssessmentSelections = 16;

} // namespace neverd::limits::web_tool

#endif // NEVERD_TOOLS_COMMON_WEB_RESOURCELIMITS_H

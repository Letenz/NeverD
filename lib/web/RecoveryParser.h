//===- RecoveryParser.h - Bounded source recovery parsing --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded source recovery parsing.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceLimits.h"

namespace neverd::web {
// An explicit sequential export profile; these models never enter the
// interactive session caches or its semantic consumers.
SourceAnalysis inspectRecoveryJavaScript(std::string_view ArtifactID,
                                         std::string_view Bytes,
                                         std::string_view SourceType);
} // namespace neverd::web

// Preserve interface spellings; resource values are defined by Limits.h.
namespace neverd::web {
inline constexpr uint64_t MaxRecoverySourceBytes =
    ::neverd::limits::web::kRecoveryParserMaxRecoverySourceBytes;
inline constexpr uint64_t MaxRecoveryNodes =
    ::neverd::limits::web::kRecoveryParserMaxRecoveryNodes;
inline constexpr uint64_t MaxRecoveryLexemes =
    ::neverd::limits::web::kRecoveryParserMaxRecoveryLexemes;
inline constexpr uint64_t MaxRecoveryStringUnits =
    ::neverd::limits::web::kRecoveryParserMaxRecoveryStringUnits;
} // namespace neverd::web

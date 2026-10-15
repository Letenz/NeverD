//===- SourceLimits.cpp - Shared source analysis resource profiles -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit source/model limits shared by parser and semantic consumers.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceLimits.h"

#include "RecoveryParser.h"

#include "neverd/web/Error.h"

#include <limits>

namespace neverd::web {

SourceLimits sourceLimits(SourceAnalysisMode Mode) {
  switch (Mode) {
  case SourceAnalysisMode::Interactive:
    return {JavaScriptParserProfile,
            MaxJavaScriptBytes,
            MaxJavaScriptNodes,
            MaxJavaScriptLexemes,
            MaxJavaScriptStringUnits,
            128 * 1024 * 1024,
            1,
            32 * 1024 * 1024,
            200000,
            5};
  case SourceAnalysisMode::Large:
    return {LargeJavaScriptParserProfile,
            8 * 1024 * 1024,
            1500000,
            2000000,
            16 * 1024 * 1024,
            768 * 1024 * 1024,
            16,
            256 * 1024 * 1024,
            1600000,
            30};
  case SourceAnalysisMode::Recovery:
    return {JavaScriptRecoveryProfile,
            MaxRecoverySourceBytes,
            MaxRecoveryNodes,
            MaxRecoveryLexemes,
            MaxRecoveryStringUnits,
            1024 * 1024 * 1024,
            1,
            256 * 1024 * 1024,
            1600000,
            30};
  }
  throw Error("unsupported_source_analysis_mode");
}

SourceLimits sourceLimits(const SourceAnalysis &Source) {
  if (Source.Mode == SourceAnalysisMode::Recovery)
    throw Error("recovery_source_model_not_analyzable");
  return sourceLimits(Source.Mode);
}

uint64_t sourceWorkLimit(const SourceAnalysis &Source, uint64_t Interactive) {
  const auto Multiplier = sourceLimits(Source).WorkMultiplier;
  if (Interactive > std::numeric_limits<uint64_t>::max() / Multiplier)
    throw Error("invalid_source_work_limit");
  return Interactive * Multiplier;
}

} // namespace neverd::web

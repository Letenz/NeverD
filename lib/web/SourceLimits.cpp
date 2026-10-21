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

#include "neverd/web/Error.h"

#include <limits>

namespace neverd::web {

SourceLimits sourceLimits(SourceAnalysisMode Mode) {
  switch (Mode) {
  case SourceAnalysisMode::Interactive:
    return InteractiveSourceLimits;
  case SourceAnalysisMode::Large:
    return LargeSourceLimits;
  case SourceAnalysisMode::Recovery:
    return RecoverySourceLimits;
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

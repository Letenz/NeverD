//===- SourceLimits.h - Source analysis profile admission ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared source and work limits for explicit analysis profiles.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Source.h"

namespace neverd::web {

inline constexpr std::string_view LargeJavaScriptParserProfile =
    "hermes-602befee-large-source-js-v1";

struct SourceLimits {
  std::string_view Profile;
  uint64_t Bytes, Nodes, Lexemes, StringUnits;
  uint64_t ModelBytes, WorkMultiplier;
  uint64_t ParserBytes, ParserAllocations, ParserSeconds;
};

/// Recovery permits bounded source export only; semantic consumers reject its
/// model. Recovery owners do not change semantic-consumer admission.
SourceLimits sourceLimits(SourceAnalysisMode Mode);
SourceLimits sourceLimits(const SourceAnalysis &Source);
uint64_t sourceWorkLimit(const SourceAnalysis &Source, uint64_t Interactive);

} // namespace neverd::web

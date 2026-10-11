//===- Rules.h - Versioned Node API syntax classification ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Match complete module/member chains without inferring runtime objects.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceOrigins.h"

namespace neverd::web::node_behavior {

struct Rule {
  std::string_view API, Kind;
  bool EncodedData = false;
};

void charge(uint64_t &Steps, uint64_t Count = 1);
std::string analysisID(std::string_view SourceID);
Rule match(const SourceOrigin &Origin, const SourceModuleAnalysis &Modules,
           uint64_t &Steps);

} // namespace neverd::web::node_behavior

//===- SourceStructure.h - Request syntax structure projection ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared bounded projection context for request values and URL components.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PassiveReport.h"

namespace neverd::web::passive_report {
struct RequestStructure {
  const SourceAnalysis &Source;
  const SourceInterface &Request;
  PassiveSourceEvidence &Result;

  void step(uint64_t Count = 1);
  uint32_t child(uint32_t Node, std::string_view Field, uint32_t Ordinal = 0);
  PassiveReportRecord record(uint32_t Node, std::string Domain,
                             std::string Edge, std::string Parent,
                             uint64_t Ordinal);
  std::string publish(PassiveReportRecord Record);
  std::string value(uint32_t Node, std::string Domain, std::string Edge,
                    std::string Parent = "", uint64_t Ordinal = 0,
                    uint32_t Depth = 0, std::string NameClass = "withheld");
  void url();
};

void projectRequestStructure(const SourceAnalysis &Source,
                             const SourceInterfaces &Interfaces,
                             const InterfaceSourceRelations &Relations,
                             PassiveSourceEvidence &Result);
} // namespace neverd::web::passive_report

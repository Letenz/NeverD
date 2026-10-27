//===- SourceOrigins.h - Syntactic module provenance -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Syntactic module provenance.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/SourceModules.h"

#include <memory>

namespace neverd::web {
inline constexpr std::string_view JavaScriptOriginProfile =
    "javascript-module-origin-candidates-v2";

/// A syntactic chain back to a module request, never a runtime object/value.
/// Names stay private; consumers publish their own fixed operation categories.
struct SourceOrigin {
  uint32_t Request = NoSourceIndex;
  // Exact ESM import occurrence, retained across aliases/member projections.
  // Distinguishes a default import from a CJS or namespace .default member.
  uint32_t Import = NoSourceIndex;
  uint32_t Construction = NoSourceIndex;
  std::vector<std::u16string> Members;
  std::string Evidence;
};
struct SourceOrigins {
  std::string ID, SourceID, BindingID, ModuleID, Status, Reason;
  std::vector<std::shared_ptr<const SourceOrigin>> Nodes;
  uint64_t Steps = 0, AllocatedUnits = 0;
};

/// Import bindings, literal require candidates, unwritten variable
/// initializers, finite object destructuring and static member chains.
/// Calls do not propagate values. New expressions retain a construction
/// boundary rather than pretending that the instance is its constructor.
SourceOrigins analyzeSourceOrigins(const SourceAnalysis &Source,
                                   const SourceBindingAnalysis &Bindings,
                                   const SourceModuleAnalysis &Modules);
} // namespace neverd::web

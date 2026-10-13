//===- SourceModel.h - Validated JavaScript source models --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Validated JavaScript source models.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Source.h"
#include "neverd/web/SourceLimits.h"

namespace neverd::web {

// All semantic consumers share the parser-model admission boundary. Returns
// the number of checked edges so each analysis can charge its own work budget.
uint64_t validateSourceModel(const SourceAnalysis &Source);

struct SourceBindingAnalysis;
struct SourceDeclarationIndex {
  std::vector<uint32_t> UniqueBindings;
  uint64_t Steps = 0;
};

// A syntax node can legitimately declare several bindings (the outer and
// inner names of a class declaration). A consumer needing one binding gets
// NoSourceIndex for multiple or conflicting identities, independent of order.
// Full declaration occurrences remain in SourceBindingAnalysis.
SourceDeclarationIndex
indexSourceDeclarations(const SourceAnalysis &Source,
                        const SourceBindingAnalysis &Bindings);

} // namespace neverd::web

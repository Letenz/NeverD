//===- Publication.h - Configuration order metadata ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed roles, source order and opaque anchors; no configuration values.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/ConfigurationOrder.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::configuration_order {
llvm::json::Object summary(const ConfigurationSourceOrder &Order);
llvm::json::Object record(const ConfigurationObjectOrder &Object,
                          const SourceAnalysis &Source,
                          const SourceBindingAnalysis &Bindings,
                          const SourceFeatures &Features);
} // namespace neverd::web::configuration_order

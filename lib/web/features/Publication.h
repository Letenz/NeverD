//===- Publication.h - Client source pattern metadata ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Closed metadata publication for client syntax navigation.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/SourceFeatures.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::client_features {
llvm::json::Object summary(const SourceFeatures &Features);
llvm::json::Object record(const SourceFeature &Feature,
                          const SourceAnalysis &Source,
                          const SourceBindingAnalysis &Bindings,
                          const SourceNavigation &Navigation);
} // namespace neverd::web::client_features

//===- FilesPublication.h - Configuration syntax metadata ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish fixed roles and source anchors without paths, values or policy
/// claims.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/ConfigurationFiles.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::configuration_files {
llvm::json::Object summary(const ConfigurationFileNavigation &Analysis);
llvm::json::Object record(const ConfigurationFileNavigation &Analysis,
                          const SourceAnalysis &Source,
                          const SourceNavigation &Navigation, uint64_t Index);
} // namespace neverd::web::configuration_files

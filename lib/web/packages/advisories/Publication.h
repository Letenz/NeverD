//===- Publication.h - Advisory metadata publication ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared summaries and pages without package names, feed URLs or private
/// prose.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageAdvisories.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::package_advisories {
llvm::json::Object summary(const PackageAdvisory &A, uint64_t Revision);
llvm::json::Object summary(const PackageAdvisoryMatches &A, uint64_t Revision);
llvm::json::Object record(const PackageAdvisoryAffected &A);
llvm::json::Object record(const PackageAdvisorySeverity &A);
llvm::json::Object record(const PackageAdvisoryMatch &A);
} // namespace neverd::web::package_advisories

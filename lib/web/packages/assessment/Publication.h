//===- Publication.h - Shared package assessment publication ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata pages and readable reports from one immutable assessment owner.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageAssessment.h"

#include "llvm/Support/JSON.h"

#include <map>
#include <optional>

namespace neverd::web::package_assessment {
using Dispositions = std::map<std::string_view, PackageDisposition>;
Dispositions dispositions(const PackageAssessment &A,
                          const PackageReviewState &Review);
llvm::json::Object summary(const PackageAssessment &A,
                           const PackageReviewState &Review, uint64_t Revision);
llvm::json::Object record(const PackageFinding &F,
                          std::optional<PackageDisposition> D);
llvm::json::Object record(const PackageReviewDecision &D);
std::string markdown(const PackageAssessment &A,
                     const PackageReviewState &Review, uint64_t Offset,
                     uint64_t Limit);
} // namespace neverd::web::package_assessment

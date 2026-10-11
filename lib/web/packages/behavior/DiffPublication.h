//===- DiffPublication.h - Behavior comparison metadata interface -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed metadata publication for selected package source comparisons.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageBehaviorDiff.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::package_behavior_diff {
llvm::json::Object summary(const PackageBehaviorDiff &D, uint64_t Revision);
llvm::json::Object record(const PackageBehaviorSourcePair &R);
llvm::json::Object record(const PackageBehaviorPattern &R);
llvm::json::Object record(const PackageBehaviorOccurrence &R);
} // namespace neverd::web::package_behavior_diff

//===- Publication.h - Shared Node evidence metadata -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed field publication shared by source and package views.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/NodeBehavior.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::node_behavior {
llvm::json::Object summary(const NodeBehavior &Analysis, uint64_t Revision);
llvm::json::Object summary(const PackageBehavior &Analysis, uint64_t Revision);
llvm::json::Object record(const NodeBehaviorRecord &Record,
                          std::string_view SourceID,
                          std::string_view ArtifactID);
llvm::json::Object source(const PackageBehaviorSource &Source);
} // namespace neverd::web::node_behavior

//===- Publication.h - Captured metadata descriptor publication ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared time validation and original descriptor anchors without source URIs.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageEvidence.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::package_evidence {
llvm::json::Object time(const PackageEvidenceTime &T);
llvm::json::Object descriptor(const PackageEvidenceDescriptor &D);
} // namespace neverd::web::package_evidence

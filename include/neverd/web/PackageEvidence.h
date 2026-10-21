//===- PackageEvidence.h - Captured metadata source and time evidence ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Caller-supplied offline metadata provenance; no clock or trust inference.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/Limits.h"

namespace neverd::web {

inline constexpr std::string_view PackageEvidenceProfile =
    "offline-package-metadata-descriptor-v1";

struct PackageEvidenceTime {
  std::string Value, OrderKey;
  std::string Status = "not_supplied";
};
struct PackageEvidenceDescriptor {
  std::string ID, ArtifactID, BlobHash;
  // Private source label/URI. It is never contacted or published verbatim.
  std::string Source;
  PackageEvidenceTime RetrievedAt, AsOf;
  std::string Status;
  uint64_t RetainedBytes = 0, Steps = 0;
};

/// Admit a named UTC subset: Gregorian years 0001..9999, seconds 00..59,
/// optional 1..9 fractional digits, and an uppercase Z. Other timestamp forms
/// remain explicitly unavailable to chronology comparisons.
PackageEvidenceTime packageEvidenceTime(std::string_view Text);
PackageEvidenceDescriptor readPackageEvidenceDescriptor(const Artifact &Input);

} // namespace neverd::web

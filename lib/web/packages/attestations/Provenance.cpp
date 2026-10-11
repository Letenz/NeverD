//===- Provenance.cpp - Captured SLSA build metadata ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retain declared build fields and chronology without verifying SLSA levels.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

namespace neverd::web::package_attestations {
void Reader::times() {
  const auto Invalid = [](const PackageEvidenceTime &T) {
    return T.Status != "valid_utc_timestamp" && T.Status != "not_supplied";
  };
  if (Invalid(A.Started) || Invalid(A.Finished))
    A.BuildTimeStatus = "unsupported_or_invalid_build_time";
  else if (A.Started.OrderKey.empty() || A.Finished.OrderKey.empty())
    A.BuildTimeStatus = "incomplete_build_times";
  else if (A.Started.OrderKey > A.Finished.OrderKey)
    A.BuildTimeStatus = "conflicting_build_times";
  else if (A.Descriptor.Status != "declared_source_and_times")
    A.BuildTimeStatus = "source_time_coverage_unavailable";
  else if (A.Finished.OrderKey > A.Descriptor.AsOf.OrderKey)
    A.BuildTimeStatus = "build_newer_than_supplied_as_of";
  else
    A.BuildTimeStatus = "declared_build_times_consistent";
}

void Reader::predicate(const Object &O) {
  const auto *Definition = object(O, "buildDefinition");
  const auto *Run = object(O, "runDetails");
  if (Definition) {
    A.BuildType = text(*Definition, "buildType").value_or("");
    const auto *External = object(*Definition, "externalParameters");
    const auto *Internal = object(*Definition, "internalParameters");
    A.ExternalParameterCount = External ? External->size() : 0;
    A.InternalParameterCount = Internal ? Internal->size() : 0;
    if (External && Internal)
      for (const auto &[Key, Value] : *External) {
        Work.charge(llvm::StringRef(Key).size() + 1);
        A.ParameterNamesOverlap |= Internal->get(Key) != nullptr;
      }
    if (const auto *Dependencies = array(*Definition, "resolvedDependencies")) {
      A.ResolvedDependencyCount = Dependencies->size();
      for (const auto &V : *Dependencies) {
        (void)object(V);
        Work.charge(64);
      }
    }
  }
  if (Run) {
    if (const auto *Builder = object(*Run, "builder"))
      A.Builder = text(*Builder, "id").value_or("");
    if (const auto *Metadata = object(*Run, "metadata")) {
      A.Started =
          packageEvidenceTime(text(*Metadata, "startedOn", 128).value_or(""));
      A.Finished =
          packageEvidenceTime(text(*Metadata, "finishedOn", 128).value_or(""));
      times();
    }
  }
  retain(512 + A.BuildType.size() + A.Builder.size() + A.Started.Value.size() +
         A.Finished.Value.size() + A.Started.OrderKey.size() +
         A.Finished.OrderKey.size());
  if (!A.BuildType.empty() && !A.Builder.empty())
    A.PredicateStatus = A.ParameterNamesOverlap
                            ? "overlapping_parameter_declarations"
                            : "supplied_slsa_v1_metadata_subset";
}
} // namespace neverd::web::package_attestations

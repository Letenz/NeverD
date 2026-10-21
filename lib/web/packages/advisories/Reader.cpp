//===- Reader.cpp - Bounded captured OSV declaration reader --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retain selected npm advisory metadata without importing prose or fetching.
///
//===----------------------------------------------------------------------===//

#include "../evidence/Admission.h"
#include "Identity.h"
#include "SemVer.h"

#include "neverd/web/Limits.h"
#include "neverd/web/PackageAdvisories.h"

namespace neverd::web {
namespace {
using namespace package_evidence;
using Object = llvm::json::Object;

const Object &object(const llvm::json::Value &V) {
  const auto *O = V.getAsObject();
  if (!O)
    throw Error("package_advisory_invalid_object");
  return *O;
}
std::string
required(const Object &O, llvm::StringRef Key,
         size_t Limit = ::neverd::limits::web::kReaderRequiredLimit) {
  const auto Value = text(O, Key, Limit);
  if (!Value)
    throw Error("package_advisory_required_field_missing");
  return *Value;
}

class Reader {
  PackageAdvisory A;
  Budget Work;
  uint64_t Selectors = 0, Ranges = 0;
  bool TopSeverity = false, EntrySeverity = false;

  void retain(uint64_t Bytes) {
    Work.charge(Bytes);
    if (Bytes > MaxRetainedBytes - A.RetainedBytes)
      throw Error("package_advisory_metadata_budget_exceeded");
    A.RetainedBytes += Bytes;
  }
  void selector() {
    if (++Selectors > MaxPackageAdvisorySelectors)
      throw Error("package_advisory_selector_budget_exceeded");
  }
  void severity(const Object &O, std::string_view AffectedID) {
    const auto *Items = array(O, "severity");
    if (!Items)
      return;
    if (!Items->empty())
      (AffectedID.empty() ? TopSeverity : EntrySeverity) = true;
    uint64_t Index = 0;
    for (const auto &V : *Items) {
      if (A.Severities.size() >= MaxPackageAdvisorySeverities)
        throw Error("package_advisory_severity_budget_exceeded");
      const auto &O = object(V);
      PackageAdvisorySeverity S;
      S.AffectedID = AffectedID;
      S.Index = Index++;
      S.Type = required(O, "type", 128);
      S.Score = required(O, "score", 4096);
      S.Source = text(O, "source", 4096).value_or("");
      S.ID = identity("package-advisory-severity",
                      {A.ID, AffectedID, std::to_string(S.Index)});
      retain(384 + S.Type.size() + S.Score.size() + S.Source.size());
      A.Severities.push_back(std::move(S));
    }
  }
  void affected(const Object &O) {
    PackageAdvisoryAffected R;
    R.Index = A.Affected.size();
    R.ID =
        identity("package-advisory-affected", {A.ID, std::to_string(R.Index)});
    if (const auto *P = O.get("package")) {
      const auto &Package = object(*P);
      R.Ecosystem = text(Package, "ecosystem", 256).value_or("");
      R.Name = text(Package, "name", 4096).value_or("");
      R.PURL = text(Package, "purl", 4096).value_or("");
    }
    R.IdentityStatus =
        package_advisories::identityStatus(R.Ecosystem, R.Name, R.PURL);
    retain(384 + R.Ecosystem.size() + R.Name.size() + R.PURL.size());
    if (const auto *Versions = array(O, "versions"))
      for (const auto &V : *Versions) {
        selector();
        const auto S = V.getAsString();
        if (!S || S->empty() || S->size() > package_advisories::MaxSemVerBytes)
          throw Error("package_advisory_invalid_version_text");
        retain(64 + S->size());
        R.Versions.push_back(S->str());
      }
    if (const auto *Items = array(O, "ranges"))
      for (const auto &V : *Items) {
        if (++Ranges > MaxPackageAdvisorySelectors)
          throw Error("package_advisory_range_budget_exceeded");
        const auto &Range = object(V);
        PackageAdvisoryRange S;
        S.Type = required(Range, "type", 128);
        retain(128 + S.Type.size());
        const auto *Events = array(Range, "events");
        if (!Events)
          throw Error("package_advisory_events_missing");
        for (const auto &V : *Events) {
          selector();
          const auto &Event = object(V);
          if (Event.size() != 1)
            throw Error("package_advisory_invalid_event");
          const auto &[Key, Value] = *Event.begin();
          const auto Text = Value.getAsString();
          if ((Key != "introduced" && Key != "fixed" &&
               Key != "last_affected" && Key != "limit") ||
              !Text || Text->empty() ||
              Text->size() > package_advisories::MaxSemVerBytes)
            throw Error("package_advisory_invalid_event");
          retain(128 + llvm::StringRef(Key).size() + Text->size());
          S.Events.push_back({Key.str(), Text->str()});
        }
        R.Ranges.push_back(std::move(S));
      }
    severity(O, R.ID);
    A.Affected.push_back(std::move(R));
  }
  void times() {
    A.TemporalStatus = "declared_times_consistent";
    const auto Invalid = [](const auto &T) {
      return T.Status != "valid_utc_timestamp" && T.Status != "not_supplied";
    };
    if (Invalid(A.Modified) || Invalid(A.Published) || Invalid(A.Withdrawn)) {
      A.TemporalStatus = "unsupported_or_invalid_advisory_time";
      return;
    }
    if ((!A.Published.OrderKey.empty() &&
         A.Published.OrderKey > A.Modified.OrderKey) ||
        (!A.Withdrawn.OrderKey.empty() &&
         (A.Withdrawn.OrderKey > A.Modified.OrderKey ||
          (!A.Published.OrderKey.empty() &&
           A.Withdrawn.OrderKey < A.Published.OrderKey)))) {
      A.TemporalStatus = "conflicting_advisory_times";
      return;
    }
    if (A.Descriptor.Status != "declared_source_and_times") {
      A.TemporalStatus = "feed_time_coverage_unavailable";
      return;
    }
    const auto &AsOf = A.Descriptor.AsOf.OrderKey;
    if (A.Modified.OrderKey > AsOf ||
        (!A.Published.OrderKey.empty() && A.Published.OrderKey > AsOf))
      A.TemporalStatus = "advisory_newer_than_supplied_as_of";
    else if (!A.Withdrawn.OrderKey.empty())
      A.TemporalStatus = "withdrawn_at_supplied_as_of";
    else if (A.Published.OrderKey.empty())
      A.TemporalStatus = "publication_time_not_supplied";
  }

public:
  PackageAdvisory read(const Artifact &Input,
                       const PackageEvidenceDescriptor &Descriptor) {
    requireIdentity(Descriptor.ID);
    requireIdentity(Descriptor.ArtifactID);
    requireIdentity(Descriptor.BlobHash);
    if (Descriptor.ID != identity("package-evidence-descriptor",
                                  {PackageEvidenceProfile,
                                   Descriptor.ArtifactID, Descriptor.BlobHash}))
      throw Error("package_advisory_descriptor_mismatch");
    A.Descriptor = Descriptor;
    retain(Descriptor.RetainedBytes);
    const auto V = document(Input, Work);
    const auto &O = *V.getAsObject();
    A.ArtifactID = Input.ID;
    A.BlobHash = Input.BlobHash;
    A.ID = identity("package-advisory", {PackageAdvisoryProfile, A.ArtifactID,
                                         A.BlobHash, Descriptor.ID});
    A.DeclaredID = required(O, "id", 256);
    A.SchemaVersion = text(O, "schema_version", 128).value_or("1.0.0");
    const auto Schema = package_advisories::parseSemVer(A.SchemaVersion);
    const auto MaxSchema = package_advisories::parseSemVer("1.9.1");
    A.SchemaStatus =
        !Schema ? "invalid_schema_version"
        : Schema->Core[0] != "1" || !Schema->Prerelease.empty() ||
                package_advisories::compareSemVer(*Schema, *MaxSchema) > 0
            ? "unsupported_schema_version"
            : "admitted_schema_subset";
    A.Modified = packageEvidenceTime(required(O, "modified", 128));
    A.Published = packageEvidenceTime(text(O, "published", 128).value_or(""));
    A.Withdrawn = packageEvidenceTime(text(O, "withdrawn", 128).value_or(""));
    retain(1024 + A.DeclaredID.size() + A.SchemaVersion.size());
    severity(O, {});
    if (const auto *Items = array(O, "affected")) {
      if (Items->size() > MaxPackageAdvisoryEntries)
        throw Error("package_advisory_affected_budget_exceeded");
      for (const auto &V : *Items)
        affected(object(V));
    }
    A.SeverityStatus = TopSeverity && EntrySeverity
                           ? "conflicting_severity_scopes"
                       : A.Severities.empty() ? "not_supplied"
                                              : "supplied_unvalidated";
    times();
    A.Steps = Work.Steps;
    return std::move(A);
  }
};
} // namespace

PackageAdvisory
readPackageAdvisory(const Artifact &Input,
                    const PackageEvidenceDescriptor &Descriptor) {
  return Reader().read(Input, Descriptor);
}
} // namespace neverd::web

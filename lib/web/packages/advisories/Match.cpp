//===- Match.cpp - Conservative OSV declaration matching -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Match exact declared npm identities and admitted selectors with work bounds.
///
//===----------------------------------------------------------------------===//

#include "../evidence/Admission.h"
#include "SemVer.h"

#include "neverd/web/PackageAdvisories.h"

#include <algorithm>

namespace neverd::web {
namespace {
using namespace package_advisories;
using package_evidence::Budget;
using package_evidence::requireIdentity;

struct Version {
  SemVer Value;
  size_t Bytes = 0;
};
std::optional<Version> version(std::string_view Text, Budget &Work) {
  Work.charge(Text.size() + 1);
  auto V = parseSemVer(Text);
  if (!V)
    return {};
  return Version{std::move(*V), Text.size()};
}
int compare(const Version &B, const Version &A, Budget &Work) {
  Work.charge(B.Bytes + A.Bytes + 1);
  return compareSemVer(B.Value, A.Value);
}
struct Event {
  const PackageAdvisoryEvent *Source = nullptr;
  std::optional<Version> V;
};
struct Range {
  bool Supported = false, InfiniteLimit = false;
  std::vector<Event> Events;
  std::vector<Version> Limits;
};

Range compileRange(const PackageAdvisoryRange &Source, Budget &Work) {
  Range R;
  if (Source.Type != "SEMVER")
    return R;
  bool Introduced = false, Fixed = false, Last = false, HasLimit = false;
  for (const auto &S : Source.Events) {
    Work.charge(128 + S.Kind.size());
    if (S.Kind == "limit") {
      HasLimit = true;
      if (S.Version == "*")
        R.InfiniteLimit = true;
      else if (auto V = version(S.Version, Work))
        R.Limits.push_back(std::move(*V));
      else
        return R;
      continue;
    }
    Introduced |= S.Kind == "introduced";
    Fixed |= S.Kind == "fixed";
    Last |= S.Kind == "last_affected";
    if (S.Kind != "introduced" && S.Kind != "fixed" &&
        S.Kind != "last_affected")
      return R;
    Event E{&S, {}};
    if (S.Kind != "introduced" || S.Version != "0") {
      E.V = version(S.Version, Work);
      if (!E.V)
        return R;
    }
    R.Events.push_back(std::move(E));
  }
  if (!Introduced || (Fixed && Last))
    return R;
  R.InfiniteLimit |= !HasLimit;
  auto Order = [&](const Event &B, const Event &A) {
    Work.charge();
    if (!B.V || !A.V)
      return B.V ? 1 : A.V ? -1 : 0;
    return compare(*B.V, *A.V, Work);
  };
  std::sort(R.Events.begin(), R.Events.end(),
            [&](const auto &B, const auto &A) { return Order(B, A) < 0; });
  for (size_t I = 1; I < R.Events.size(); ++I)
    if (Order(R.Events[I - 1], R.Events[I]) == 0 &&
        R.Events[I - 1].Source->Kind != R.Events[I].Source->Kind)
      return R;
  R.Supported = true;
  return R;
}

bool included(const Version &V, const Range &R, Budget &Work) {
  bool BeforeLimit = R.InfiniteLimit;
  for (const auto &Limit : R.Limits)
    BeforeLimit |= compare(V, Limit, Work) < 0;
  if (!BeforeLimit)
    return false;
  bool Affected = false;
  for (const auto &E : R.Events) {
    const auto C = E.V ? compare(V, *E.V, Work) : 1;
    if (E.Source->Kind == "introduced" && C >= 0)
      Affected = true;
    else if ((E.Source->Kind == "fixed" && C >= 0) ||
             (E.Source->Kind == "last_affected" && C > 0))
      Affected = false;
  }
  return Affected;
}

struct Affected {
  const PackageAdvisoryAffected *Source;
  std::vector<Range> Ranges;
  bool Complete = true;
};
struct Advisory {
  const PackageAdvisory *Source;
  std::vector<Affected> Entries;
};

class Matcher {
  const PackageAnalysis &P;
  Budget Work;
  PackageAdvisoryMatches Result;
  uint64_t Anchors = 0;

  Advisory compile(const PackageAdvisory &A) {
    requireIdentity(A.ID);
    if (A.ID !=
            identity("package-advisory", {PackageAdvisoryProfile, A.ArtifactID,
                                          A.BlobHash, A.Descriptor.ID}) ||
        A.Affected.size() > MaxPackageAdvisoryEntries)
      throw Error("package_advisory_selection_mismatch");
    Advisory C{&A, {}};
    uint64_t Selectors = 0, Ranges = 0;
    for (const auto &E : A.Affected) {
      Work.charge(256 + E.Name.size() + E.Ecosystem.size() + E.PURL.size());
      if (E.Index != C.Entries.size() ||
          E.ID != identity("package-advisory-affected",
                           {A.ID, std::to_string(E.Index)}))
        throw Error("package_advisory_affected_mismatch");
      Affected F{&E, {}, true};
      if (E.Versions.size() > MaxPackageAdvisorySelectors - Selectors)
        throw Error("package_advisory_selector_budget_exceeded");
      Selectors += E.Versions.size();
      for (const auto &V : E.Versions)
        F.Complete &= bool(version(V, Work));
      for (const auto &R : E.Ranges) {
        if (++Ranges > MaxPackageAdvisorySelectors ||
            R.Events.size() > MaxPackageAdvisorySelectors - Selectors)
          throw Error("package_advisory_selector_budget_exceeded");
        Selectors += R.Events.size();
        auto Compiled = compileRange(R, Work);
        F.Complete &= Compiled.Supported;
        F.Ranges.push_back(std::move(Compiled));
      }
      F.Complete &= !E.Versions.empty() || !E.Ranges.empty();
      C.Entries.push_back(std::move(F));
    }
    return C;
  }
  void append(PackageAdvisoryMatch R) {
    if (Result.Records.size() >= MaxPackageAdvisoryMatches)
      throw Error("package_advisory_match_budget_exceeded");
    Work.charge(512);
    R.ID = identity("package-advisory-match",
                    {Result.ID, R.PackageID, R.AdvisoryID});
    Result.MatchCount += R.Status == "declared_version_match";
    Result.UnknownCount += R.Status == "coverage_unknown";
    Result.Records.push_back(std::move(R));
  }
  void match(const PackageInstance &Instance, const Advisory &A) {
    PackageAdvisoryMatch R;
    R.PackageID = Instance.ID;
    R.AdvisoryID = A.Source->ID;
    R.Status = "coverage_unknown";
    R.IdentityStatus = "declared_package_identity";
    R.VersionStatus = "strict_semver_declaration";
    Work.charge(Instance.Name ? Instance.Name->size() + 1 : 1);
    const auto V = Instance.Version ? version(*Instance.Version, Work)
                                    : std::optional<Version>{};
    if (P.LegacyStatus == "conflicting_declarations" ||
        Instance.ManifestStatus == "conflicting_declarations")
      R.IdentityStatus = "conflicting_package_declarations";
    else if (!Instance.Name || Instance.Name->empty() ||
             (Instance.NameSource != "manifest" &&
              Instance.NameSource != "lockfile"))
      R.IdentityStatus = "declared_package_name_unavailable";
    if (Instance.VersionKind != "package_version_declaration" || !V)
      R.VersionStatus = "package_version_unavailable_or_unsupported";
    if (R.IdentityStatus != "declared_package_identity" ||
        R.VersionStatus != "strict_semver_declaration" ||
        A.Source->SchemaStatus != "admitted_schema_subset") {
      append(std::move(R));
      return;
    }
    bool Unknown = A.Entries.empty(), MatchingName = false;
    for (const auto &E : A.Entries) {
      const auto &S = *E.Source;
      Work.charge(Instance.Name->size() + S.Name.size() + 64);
      if (S.Ecosystem.empty() || S.Name.empty()) {
        Unknown = true;
        continue;
      }
      if (S.Ecosystem != "npm")
        continue;
      if (!S.PURL.empty() &&
          S.IdentityStatus != "matching_versionless_npm_purl") {
        Unknown = true;
        continue;
      }
      if (S.Name != "*" && S.Name != *Instance.Name)
        continue;
      MatchingName = true;
      Unknown |= !E.Complete;
      bool Match = false;
      for (const auto &Enumerated : S.Versions) {
        Work.charge(Instance.Version->size() + Enumerated.size() + 1);
        Match |= *Instance.Version == Enumerated;
      }
      for (const auto &Range : E.Ranges)
        if (Range.Supported)
          Match |= included(*V, Range, Work);
      if (Match) {
        if (++Anchors > MaxPackageAdvisoryMatches)
          throw Error("package_advisory_anchor_budget_exceeded");
        Work.charge(128);
        R.MatchedAffectedIDs.push_back(S.ID);
      }
    }
    R.SelectorCoverageComplete = !Unknown;
    if (!R.MatchedAffectedIDs.empty())
      R.Status = "declared_version_match";
    else if (!Unknown)
      R.Status = MatchingName ? "version_not_in_admitted_selectors"
                              : "no_matching_package_declaration";
    append(std::move(R));
  }

public:
  explicit Matcher(const PackageAnalysis &P) : P(P) {}
  PackageAdvisoryMatches run(std::vector<const PackageAdvisory *> Inputs) {
    requireIdentity(P.ID);
    if ((P.Kind != "npm-lock" && P.Kind != "package-json") ||
        P.Packages.empty() || P.Packages.size() > MaxPackageInstances ||
        Inputs.size() > MaxPackageAdvisoryImports)
      throw Error("package_advisory_selection_budget_exceeded");
    for (const auto *A : Inputs)
      if (!A)
        throw Error("package_advisory_selection_mismatch");
    std::sort(Inputs.begin(), Inputs.end(), [&](const auto *B, const auto *A) {
      Work.charge(B->ID.size() + A->ID.size() + 1);
      return B->ID < A->ID;
    });
    std::vector<std::string_view> Key{PackageAdvisoryProfile, P.ID};
    std::vector<Advisory> Compiled;
    for (const auto *A : Inputs) {
      if (!Result.AdvisoryIDs.empty() && Result.AdvisoryIDs.back() == A->ID)
        throw Error("package_advisory_duplicate_selection");
      Key.push_back(A->ID);
      Result.AdvisoryIDs.push_back(A->ID);
      Compiled.push_back(compile(*A));
    }
    Result.ID = identity("package-advisory-matches", Key);
    Result.PackageAnalysisID = P.ID;
    for (size_t I = 0; I < P.Packages.size(); ++I) {
      const auto &Instance = P.Packages[I];
      if (Instance.ID !=
          identity("package-instance", {P.ID, std::to_string(I)}))
        throw Error("package_advisory_instance_mismatch");
      for (const auto &A : Compiled)
        match(Instance, A);
    }
    Result.Steps = Work.Steps;
    return std::move(Result);
  }
};
} // namespace

PackageAdvisoryMatches
matchPackageAdvisories(const PackageAnalysis &Package,
                       const std::vector<const PackageAdvisory *> &Advisories) {
  return Matcher(Package).run(Advisories);
}
} // namespace neverd::web

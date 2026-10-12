//===- Internal.h - Package assessment composition owners --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded indexes and typed joins shared by the assessment record producers.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "../evidence/Admission.h"

#include "neverd/web/PackageAdvisories.h"
#include "neverd/web/PackageArchive.h"
#include "neverd/web/PackageAssessment.h"
#include "neverd/web/PackageAttestations.h"
#include "neverd/web/PackageBehaviorDiff.h"
#include "neverd/web/PackageInstall.h"

#include <map>

namespace neverd::web::package_assessment {
class Composer {
public:
  const PackageAnalysis &P;
  const Snapshot &Namespace;
  const PackageAssessmentSelection &Selection;
  PackageAssessment A;
  const PackageArchive *Archive = nullptr;
  const PackageInstance *Root = nullptr;
  std::map<std::string_view, const Artifact *> Members, Originals;
  std::map<std::string_view, const PackageArchiveMember *> ArchiveMembers;
  std::map<std::string_view, const PackageInstance *> Instances;
  std::map<std::string_view, const PackageAttestation *> Attestations;
  std::map<std::string_view, const PackageAdvisory *> Advisories;
  std::map<std::string_view, const PackageIntegrityResult *> Integrities;
  std::map<std::string_view, const PackageAttestationDigestMatch *>
      DigestMatches;

  Composer(const PackageAnalysis &P, const Snapshot &Namespace,
           const PackageAssessmentSelection &Selection);
  void charge(uint64_t Amount = 1);
  void retain(uint64_t Amount);
  void append(PackageFinding F);
  void coverage(std::string_view Surface, std::string_view OwnerID);
  void feed(std::string_view OwnerID, const PackageEvidenceDescriptor &D);
  const PackageInstance &instance(std::string_view ID);
  bool originalContainsManifest(const PackageIntegrityResult &R,
                                const PackageInstance &I);
  void integrityAnchor(const PackageIntegrityResult &R);
  void inventory();
  void integrity();
  void metadata();
  void behavior();
  PackageAssessment run();
};
} // namespace neverd::web::package_assessment

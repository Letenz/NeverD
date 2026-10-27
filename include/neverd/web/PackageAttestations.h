//===- PackageAttestations.h - Offline attestation metadata --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline declarations and explicit digest relationships without trust claims.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Limits.h"
#include "neverd/web/PackageEvidence.h"
#include "neverd/web/PackageIntegrity.h"

#include <optional>
#include <vector>

namespace neverd::web {

inline constexpr std::string_view PackageAttestationProfile =
    "offline-package-attestation-metadata-v1";

// Supplied names, URIs, digests, payloads and signatures are private model
// fields. Public records expose fixed states and original evidence anchors.
struct PackageAttestationSignature {
  std::string ID, KeyID, Bytes, EncodingStatus;
  uint64_t Index = 0;
};
struct PackageAttestationDigest {
  std::string ID, SubjectID, Algorithm, Value, Validation;
  uint64_t Index = 0;
};
struct PackageAttestationSubject {
  std::string ID, Name, URI;
  std::vector<std::string> DigestIDs;
  uint64_t Index = 0;
};
struct PackageAttestation {
  std::string ID, ArtifactID, BlobHash, Kind;
  PackageEvidenceDescriptor Descriptor;
  std::string Name, Version, PackageIdentityStatus;
  std::optional<std::string> Integrity;
  std::string IntegrityStatus, AttestationReference;
  std::string PayloadType, Payload, PayloadHash, PayloadStatus;
  std::string StatementType, StatementStatus, PredicateType, PredicateStatus;
  std::string Builder, BuildType, BuildTimeStatus;
  PackageEvidenceTime Started, Finished;
  uint64_t ExternalParameterCount = 0, InternalParameterCount = 0;
  uint64_t ResolvedDependencyCount = 0;
  bool ParameterNamesOverlap = false;
  std::vector<PackageAttestationSignature> Signatures;
  std::vector<PackageAttestationSubject> Subjects;
  std::vector<PackageAttestationDigest> Digests;
  uint64_t RetainedBytes = 0, Steps = 0;
};
struct PackageAttestationDigestMatch {
  std::string ID, AttestationID, SubjectID, DigestID;
  PackageIntegrityResult Integrity;
};

/// Kind explicitly selects registry-dist, intoto-statement or dsse-intoto.
/// The reader never follows a URL, loads a key or authenticates a signature.
PackageAttestation
readPackageAttestation(const Artifact &Input,
                       const PackageEvidenceDescriptor &Descriptor,
                       std::string_view Kind);
/// Compare one explicit SHA-256/SHA-512 subject declaration with original bytes
/// through the existing integrity owner. Other subjects/digests are not
/// checked; equality does not authenticate the statement, signer or package
/// inventory.
PackageAttestationDigestMatch
matchPackageAttestationDigest(const Artifact &Original,
                              const PackageAttestation &Attestation,
                              std::string_view DigestID);

} // namespace neverd::web

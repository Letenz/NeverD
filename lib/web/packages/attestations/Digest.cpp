//===- Digest.cpp - Explicit attestation subject byte comparison ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Reuse original SRI verification for one named digest; no signature claim.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include "llvm/Support/Base64.h"

#include <algorithm>

namespace neverd::web::package_attestations {
std::string_view digestStatus(std::string_view Algorithm,
                              std::string_view Value) {
  const auto Length = Algorithm == "sha256"   ? 64U
                      : Algorithm == "sha512" ? 128U
                                              : 0U;
  if (!Length)
    return "unsupported_digest_algorithm";
  return Value.size() == Length &&
                 Value.find_first_not_of("0123456789abcdefABCDEF") == Value.npos
             ? "admitted_hex_digest"
             : "invalid_hex_digest";
}
} // namespace neverd::web::package_attestations

namespace neverd::web {
PackageAttestationDigestMatch
matchPackageAttestationDigest(const Artifact &Original,
                              const PackageAttestation &A,
                              std::string_view DigestID) {
  using package_evidence::requireIdentity;
  requireIdentity(A.ID);
  requireIdentity(A.ArtifactID);
  requireIdentity(A.BlobHash);
  requireIdentity(A.Descriptor.ID);
  requireIdentity(Original.ID);
  requireIdentity(Original.BlobHash);
  requireIdentity(DigestID);
  if (A.ID != identity("package-attestation",
                       {PackageAttestationProfile, A.Kind, A.ArtifactID,
                        A.BlobHash, A.Descriptor.ID}) ||
      (A.Kind != "intoto-statement" && A.Kind != "dsse-intoto") ||
      A.StatementStatus != "admitted_statement_v1_subset" ||
      A.Subjects.size() > MaxPackageAttestationSubjects ||
      A.Digests.size() > MaxPackageAttestationDigests)
    throw Error("package_attestation_subject_selection_unavailable");
  const auto Found =
      std::find_if(A.Digests.begin(), A.Digests.end(),
                   [&](const auto &D) { return D.ID == DigestID; });
  if (Found == A.Digests.end())
    throw Error("unknown_package_attestation_digest");
  const auto &D = *Found;
  const auto Subject =
      std::find_if(A.Subjects.begin(), A.Subjects.end(),
                   [&](const auto &S) { return S.ID == D.SubjectID; });
  if (Subject == A.Subjects.end() ||
      D.Index != uint64_t(Found - A.Digests.begin()) ||
      Subject->Index != uint64_t(Subject - A.Subjects.begin()) ||
      Subject->DigestIDs.size() > MaxPackageAttestationDigests ||
      Subject->ID != identity("package-attestation-subject",
                              {A.ID, std::to_string(Subject->Index)}) ||
      std::find(Subject->DigestIDs.begin(), Subject->DigestIDs.end(), D.ID) ==
          Subject->DigestIDs.end() ||
      D.ID != identity("package-attestation-digest",
                       {A.ID, Subject->ID, std::to_string(D.Index)}))
    throw Error("package_attestation_subject_selection_mismatch");
  const auto Status = package_attestations::digestStatus(D.Algorithm, D.Value);
  if (Status != "admitted_hex_digest" || D.Validation != Status)
    throw Error("package_attestation_digest_unavailable");
  const auto Digit = [](char C) -> unsigned {
    return C <= '9' ? C - '0' : C <= 'F' ? C - 'A' + 10 : C - 'a' + 10;
  };
  std::string Raw;
  for (size_t I = 0; I < D.Value.size(); I += 2)
    Raw.push_back(char((Digit(D.Value[I]) << 4) | Digit(D.Value[I + 1])));
  const PackageIntegrityDeclaration Declaration{A.ArtifactID,
                                                A.BlobHash,
                                                D.ID,
                                                "intoto_subject_digest",
                                                D.Algorithm + "-" +
                                                    llvm::encodeBase64(Raw),
                                                false};
  PackageAttestationDigestMatch M;
  M.AttestationID = A.ID;
  M.SubjectID = Subject->ID;
  M.DigestID = D.ID;
  M.Integrity = verifyPackageIntegrity(Original, Declaration);
  M.ID = identity("package-attestation-digest-match",
                  {PackageAttestationProfile, A.ID, D.ID, M.Integrity.ID,
                   PackageIntegrityProfile});
  return M;
}
} // namespace neverd::web

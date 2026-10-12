//===- Publication.cpp - Attestation metadata without trust claims ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// One public representation for captured declarations and original byte joins.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

#include "../evidence/Publication.h"

namespace neverd::web::package_attestations {
namespace {
llvm::json::Object base(uint64_t Revision) {
  return llvm::json::Object{{"schema_version", 1},
                            {"status", "ok"},
                            {"revision", std::to_string(Revision)},
                            {"profile", std::string(PackageAttestationProfile)},
                            {"analysis_status", "partial"},
                            {"signature_verification", "not_performed"},
                            {"publisher_authenticated", false},
                            {"trust_chain_verified", false},
                            {"transparency_verified", false},
                            {"slsa_level_verified", false},
                            {"freshness_assessed", false},
                            {"executes_input", false},
                            {"fetches_remote_data", false},
                            {"safety_verdict", "not_assessed"},
                            {"redaction_policy", "metadata-only-v1"}};
}
llvm::json::Array anchors(const std::vector<std::string> &IDs) {
  llvm::json::Array A;
  for (const auto &ID : IDs)
    A.emplace_back(ID);
  return A;
}
} // namespace

llvm::json::Object summary(const PackageAttestation &A, uint64_t Revision) {
  auto R = base(Revision);
  R["package_attestation_id"] = A.ID;
  R["artifact_id"] = A.ArtifactID;
  R["blob_sha256"] = A.BlobHash;
  R["input_kind"] = A.Kind;
  R["descriptor"] = package_evidence::descriptor(A.Descriptor);
  R["package_identity_status"] = A.PackageIdentityStatus;
  R["integrity_declaration_status"] = A.IntegrityStatus;
  R["attestation_reference_present"] = !A.AttestationReference.empty();
  R["payload_status"] = A.PayloadStatus;
  R["payload_bytes"] = A.Payload.size();
  R["statement_status"] = A.StatementStatus;
  R["predicate_status"] = A.PredicateStatus;
  R["builder_present"] = !A.Builder.empty();
  R["build_type_present"] = !A.BuildType.empty();
  R["external_parameter_count"] = A.ExternalParameterCount;
  R["internal_parameter_count"] = A.InternalParameterCount;
  R["parameter_names_overlap"] = A.ParameterNamesOverlap;
  R["resolved_dependency_count"] = A.ResolvedDependencyCount;
  R["resolved_dependency_digests_verified"] = false;
  R["build_time_status"] = A.BuildTimeStatus;
  R["started_on"] = package_evidence::time(A.Started);
  R["finished_on"] = package_evidence::time(A.Finished);
  R["signature_count"] = A.Signatures.size();
  R["subject_count"] = A.Subjects.size();
  R["digest_count"] = A.Digests.size();
  R["retained_metadata_bytes"] = A.RetainedBytes;
  R["steps"] = A.Steps;
  return R;
}
llvm::json::Object summary(const PackageAttestationDigestMatch &M,
                           uint64_t Revision) {
  auto R = base(Revision);
  const auto &I = M.Integrity;
  R["package_attestation_digest_match_id"] = M.ID;
  R["package_attestation_id"] = M.AttestationID;
  R["subject_id"] = M.SubjectID;
  R["digest_id"] = M.DigestID;
  R["integrity_id"] = I.ID;
  R["integrity_profile"] = std::string(PackageIntegrityProfile);
  R["artifact_id"] = I.ArtifactID;
  R["blob_sha256"] = I.BlobHash;
  R["original_bytes"] = std::to_string(I.Bytes);
  R["declaration_artifact_id"] = I.DeclarationArtifactID;
  R["declaration_sha256"] = I.DeclarationHash;
  R["declaration_selection_id"] = I.SelectionID;
  R["declaration_kind"] = I.Kind;
  R["byte_domain"] = "selected_original_artifact";
  R["binding"] = "caller_selected_subject_digest";
  R["integrity_status"] = I.Status;
  R["algorithm"] = I.Algorithm;
  R["bytes_match_declaration"] = I.Status == "match";
  R["other_subjects_or_digests_checked"] = false;
  R["package_inventory_bound"] = false;
  return R;
}
llvm::json::Object record(const PackageAttestationSignature &S) {
  return llvm::json::Object{{"signature_id", S.ID},
                            {"array_index", S.Index},
                            {"key_id_present", !S.KeyID.empty()},
                            {"decoded_bytes", S.Bytes.size()},
                            {"encoding_status", S.EncodingStatus},
                            {"signature_verification", "not_performed"}};
}
llvm::json::Object record(const PackageAttestationSubject &S) {
  return llvm::json::Object{
      {"subject_id", S.ID},
      {"array_index", S.Index},
      {"name_present", !S.Name.empty()},
      {"uri_present", !S.URI.empty()},
      {"digest_status", S.DigestIDs.empty() ? "not_supplied" : "declared"},
      {"digest_ids", anchors(S.DigestIDs)}};
}
llvm::json::Object record(const PackageAttestationDigest &D) {
  return llvm::json::Object{
      {"digest_id", D.ID},
      {"digest_index", D.Index},
      {"subject_id", D.SubjectID},
      {"algorithm_class", D.Algorithm == "sha256"   ? "sha256"
                          : D.Algorithm == "sha512" ? "sha512"
                                                    : "unsupported"},
      {"validation", D.Validation},
      {"byte_comparison", "not_selected_in_this_record"}};
}
} // namespace neverd::web::package_attestations

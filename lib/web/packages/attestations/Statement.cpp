//===- Statement.cpp - Captured in-toto and DSSE metadata ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode explicit statement metadata while preserving unsigned evidence
/// limits.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

#include "neverd/web/Limits.h"

#include <algorithm>

namespace neverd::web::package_attestations {
void Reader::envelope(const Object &O) {
  const auto Type = text(O, "payloadType");
  const auto Payload = text(O, "payload", MaxPackageEvidenceInputBytes);
  if (!Type || !Payload)
    throw Error("package_attestation_payload_missing");
  A.PayloadType = *Type;
  retain(A.PayloadType.size());
  signatures(O, true, true);
  Work.charge(Payload->size());
  auto Decoded =
      decodeBase64(*Payload, MaxPackageAttestationPayloadBytes, true);
  if (Decoded.Status == "decoded_byte_budget_exceeded")
    throw Error("package_attestation_payload_budget_exceeded");
  A.PayloadStatus = Decoded.Status;
  A.StatementStatus = "payload_unavailable";
  A.PredicateStatus = "payload_unavailable";
  if (Decoded.Status != "admitted_standard_base64" &&
      Decoded.Status != "admitted_url_base64")
    return;
  A.Payload = std::move(Decoded.Bytes);
  retain(A.Payload.size());
  A.PayloadHash = sha256(A.Payload);
  Work.charge(A.Payload.size());
  if (A.PayloadType != "application/vnd.in-toto+json") {
    A.PayloadStatus = "unsupported_payload_type";
    return;
  }
  uint64_t Nodes = 0;
  const auto V = parseBoundedJSON(A.Payload,
                                  {MaxPackageAttestationPayloadBytes, 32, 32768,
                                   MaxPackageAttestationPayloadBytes},
                                  &Nodes);
  Work.charge(Nodes * 16);
  statement(object(V));
}

void Reader::subject(const Object &O) {
  PackageAttestationSubject S;
  S.Index = A.Subjects.size();
  S.ID =
      identity("package-attestation-subject", {A.ID, std::to_string(S.Index)});
  S.Name = text(O, "name").value_or("");
  S.URI = text(O, "uri").value_or("");
  retain(384 + S.Name.size() + S.URI.size());
  if (const auto *Digests = object(O, "digest")) {
    if (Digests->size() > MaxPackageAttestationDigests - A.Digests.size())
      throw Error("package_attestation_digest_budget_exceeded");
    std::vector<std::pair<std::string, std::string>> Ordered;
    for (const auto &[Key, Value] : *Digests) {
      const auto Algorithm = llvm::StringRef(Key);
      const auto Digest = Value.getAsString();
      if (Algorithm.empty() ||
          Algorithm.size() > ::neverd::limits::web::
                                 kStatementReaderSubjectAlgorithmSizeLimit ||
          !Digest ||
          Digest->size() >
              ::neverd::limits::web::kStatementReaderSubjectDigestSizeLimit)
        throw Error("package_attestation_invalid_digest_field");
      Work.charge(Algorithm.size() + Digest->size());
      Ordered.emplace_back(Algorithm.str(), Digest->str());
    }
    std::sort(Ordered.begin(), Ordered.end(),
              [&](const auto &B, const auto &A) {
                Work.charge(B.first.size() + A.first.size() + 1);
                return B.first < A.first;
              });
    for (auto &[Algorithm, Value] : Ordered) {
      PackageAttestationDigest D;
      D.Index = A.Digests.size();
      D.SubjectID = S.ID;
      D.Algorithm = std::move(Algorithm);
      D.Value = std::move(Value);
      D.ID = identity("package-attestation-digest",
                      {A.ID, S.ID, std::to_string(D.Index)});
      D.Validation = digestStatus(D.Algorithm, D.Value);
      retain(384 + D.Algorithm.size() + D.Value.size());
      S.DigestIDs.push_back(D.ID);
      A.Digests.push_back(std::move(D));
    }
  }
  A.Subjects.push_back(std::move(S));
}

void Reader::statement(const Object &O) {
  A.StatementType = text(O, "_type").value_or("");
  retain(A.StatementType.size());
  A.StatementStatus = A.StatementType.empty() ? "statement_type_not_supplied"
                                              : "unsupported_statement_type";
  A.PredicateStatus = "statement_unavailable";
  if (A.StatementType != "https://in-toto.io/Statement/v1")
    return;
  A.PredicateType = text(O, "predicateType").value_or("");
  retain(A.PredicateType.size());
  const auto *Subjects = array(O, "subject");
  A.StatementStatus = !Subjects || Subjects->empty() || A.PredicateType.empty()
                          ? "incomplete_statement_v1"
                          : "admitted_statement_v1_subset";
  if (Subjects) {
    if (Subjects->size() > MaxPackageAttestationSubjects)
      throw Error("package_attestation_subject_budget_exceeded");
    for (const auto &V : *Subjects)
      subject(object(V));
    if (std::any_of(A.Subjects.begin(), A.Subjects.end(),
                    [](const auto &S) { return S.DigestIDs.empty(); }))
      A.StatementStatus = "incomplete_statement_v1";
  }
  A.PredicateStatus =
      A.PredicateType.empty() ? "not_supplied" : "unsupported_predicate_type";
  const auto *Predicate = object(O, "predicate");
  if (A.PredicateType == "https://slsa.dev/provenance/v1") {
    A.PredicateStatus = "incomplete_slsa_v1_metadata";
    if (Predicate)
      predicate(*Predicate);
  }
}
} // namespace neverd::web::package_attestations

//===- Reader.cpp - Captured attestation metadata admission ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit profiles and bounded original-byte, descriptor and signature
/// fields.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

namespace neverd::web::package_attestations {
const Object &object(const llvm::json::Value &V) {
  const auto *O = V.getAsObject();
  if (!O)
    throw Error("package_attestation_invalid_object");
  return *O;
}
const Object *object(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  return !V || V->getAsNull() ? nullptr : &object(*V);
}
const llvm::json::Array *array(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V || V->getAsNull())
    return nullptr;
  const auto *Items = V->getAsArray();
  if (!Items)
    throw Error("package_attestation_invalid_array");
  return Items;
}
std::optional<std::string> text(const Object &O, llvm::StringRef Key,
                                size_t MaxBytes) {
  const auto *V = O.get(Key);
  if (!V || V->getAsNull())
    return {};
  const auto S = V->getAsString();
  if (!S)
    throw Error("package_attestation_invalid_text");
  if (S->size() > MaxBytes)
    throw Error("package_attestation_field_budget_exceeded");
  return S->str();
}
void Reader::retain(uint64_t Bytes) {
  Work.charge(Bytes);
  if (Bytes > MaxPackageEvidenceRetainedBytes - A.RetainedBytes)
    throw Error("package_attestation_metadata_budget_exceeded");
  A.RetainedBytes += Bytes;
}
void Reader::signatures(const Object &O, bool Required, bool AllowURL) {
  const auto *Items = array(O, "signatures");
  if (!Items) {
    if (Required)
      throw Error("package_attestation_signatures_missing");
    return;
  }
  if (Items->size() > MaxPackageAttestationSignatures)
    throw Error("package_attestation_signature_budget_exceeded");
  for (const auto &V : *Items) {
    const auto &Entry = object(V);
    const auto Encoded = text(Entry, "sig", 24 * 1024);
    if (!Encoded)
      throw Error("package_attestation_signature_missing");
    Work.charge(Encoded->size());
    auto Decoded =
        decodeBase64(*Encoded, MaxPackageAttestationSignatureBytes, AllowURL);
    if (Decoded.Status == "decoded_byte_budget_exceeded")
      throw Error("package_attestation_signature_budget_exceeded");
    PackageAttestationSignature S;
    S.Index = A.Signatures.size();
    S.ID = identity("package-attestation-signature",
                    {A.ID, std::to_string(S.Index)});
    S.KeyID = text(Entry, "keyid").value_or("");
    S.Bytes = std::move(Decoded.Bytes);
    S.EncodingStatus = std::move(Decoded.Status);
    retain(384 + S.KeyID.size() + S.Bytes.size());
    A.Signatures.push_back(std::move(S));
  }
}
PackageAttestation Reader::read(const Artifact &Input,
                                const PackageEvidenceDescriptor &Descriptor,
                                std::string_view Kind) {
  if (Kind != "registry-dist" && Kind != "intoto-statement" &&
      Kind != "dsse-intoto")
    throw Error("unsupported_package_attestation_kind");
  using package_evidence::requireIdentity;
  requireIdentity(Descriptor.ID);
  requireIdentity(Descriptor.ArtifactID);
  requireIdentity(Descriptor.BlobHash);
  if (Descriptor.ID != identity("package-evidence-descriptor",
                                {PackageEvidenceProfile, Descriptor.ArtifactID,
                                 Descriptor.BlobHash}))
    throw Error("package_attestation_descriptor_mismatch");
  const auto V = package_evidence::document(Input, Work);
  A.Kind = Kind;
  A.ArtifactID = Input.ID;
  A.BlobHash = Input.BlobHash;
  A.Descriptor = Descriptor;
  A.ID = identity("package-attestation",
                  {PackageAttestationProfile, Kind, Input.ID, Input.BlobHash,
                   Descriptor.ID});
  retain(2048 + Descriptor.RetainedBytes);
  A.PackageIdentityStatus = A.IntegrityStatus = "not_applicable";
  A.PayloadStatus = A.StatementStatus = A.PredicateStatus = "not_applicable";
  A.BuildTimeStatus = "not_supplied";
  const auto &O = object(V);
  if (Kind == "registry-dist")
    registry(O);
  else if (Kind == "dsse-intoto")
    envelope(O);
  else
    statement(O);
  A.Steps = Work.Steps;
  return std::move(A);
}
} // namespace neverd::web::package_attestations

namespace neverd::web {
PackageAttestation
readPackageAttestation(const Artifact &Input,
                       const PackageEvidenceDescriptor &Descriptor,
                       std::string_view Kind) {
  return package_attestations::Reader().read(Input, Descriptor, Kind);
}
} // namespace neverd::web

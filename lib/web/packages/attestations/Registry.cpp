//===- Registry.cpp - Captured npm signature declarations -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve registry name/version, SRI and signature metadata without key
/// fetch.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"

namespace neverd::web::package_attestations {
void Reader::registry(const Object &O) {
  A.Name = text(O, "name").value_or("");
  A.Version = text(O, "version", 1024).value_or("");
  A.PackageIdentityStatus = A.Name.empty() || A.Version.empty()
                                ? "package_declaration_incomplete"
                                : "name_and_version_declared";
  if (const auto ID = text(O, "_id", 8192); ID && !ID->empty()) {
    Work.charge(ID->size() + A.Name.size() + A.Version.size());
    if (A.Name.empty() || A.Version.empty() || *ID != A.Name + "@" + A.Version)
      A.PackageIdentityStatus = "conflicting_registry_declarations";
  }
  retain(A.Name.size() + A.Version.size());
  A.IntegrityStatus = "not_supplied";
  const auto *Dist = object(O, "dist");
  if (!Dist)
    return;
  A.Integrity = text(*Dist, "integrity", 65536);
  if (A.Integrity) {
    retain(A.Integrity->size());
    A.IntegrityStatus = packageIntegrityDeclarationStatus(*A.Integrity);
  }
  signatures(*Dist, false, false);
  if (const auto *Reference = object(*Dist, "attestations")) {
    A.AttestationReference = text(*Reference, "url").value_or("");
    retain(A.AttestationReference.size());
  }
}
} // namespace neverd::web::package_attestations

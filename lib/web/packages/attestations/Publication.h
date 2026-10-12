//===- Publication.h - Offline attestation records ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Metadata publication excludes names, keys, signatures and payload contents.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/PackageAttestations.h"

#include "llvm/Support/JSON.h"

namespace neverd::web::package_attestations {
llvm::json::Object summary(const PackageAttestation &A, uint64_t Revision);
llvm::json::Object summary(const PackageAttestationDigestMatch &M,
                           uint64_t Revision);
llvm::json::Object record(const PackageAttestationSignature &S);
llvm::json::Object record(const PackageAttestationSubject &S);
llvm::json::Object record(const PackageAttestationDigest &D);
} // namespace neverd::web::package_attestations

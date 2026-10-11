//===- Encoding.h - Bounded attestation envelope encoding -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit Base64 variants for captured DSSE and registry signature bytes.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::web::package_attestations {
struct DecodedBase64 {
  std::string Bytes, Status = "invalid_base64";
};
/// Admit zero pad bits, optional padding and one unmixed alphabet. A caller
/// explicitly chooses whether the URL alphabet is allowed. Never guess hex.
DecodedBase64 decodeBase64(std::string_view Text, uint64_t MaxBytes,
                           bool AllowURL);
} // namespace neverd::web::package_attestations

//===- Internal.h - Captured attestation reader boundaries ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared bounded field admission and distinct registry/statement readers.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "../evidence/Admission.h"
#include "Encoding.h"

#include "neverd/web/Limits.h"
#include "neverd/web/PackageAttestations.h"

namespace neverd::web::package_attestations {
using Object = llvm::json::Object;
const Object &object(const llvm::json::Value &V);
const Object *object(const Object &O, llvm::StringRef Key);
const llvm::json::Array *array(const Object &O, llvm::StringRef Key);
/// These metadata profiles retain empty and missing strings without coercion;
/// nullable protobuf-style fields are distinct from OSV required fields.
std::optional<std::string>
text(const Object &O, llvm::StringRef Key,
     size_t MaxBytes = ::neverd::limits::web::kInternalTextMaxBytes);
std::string_view digestStatus(std::string_view Algorithm,
                              std::string_view Value);

class Reader {
  PackageAttestation A;
  package_evidence::Budget Work;

  void retain(uint64_t Bytes);
  void signatures(const Object &O, bool Required, bool AllowURL);
  void registry(const Object &O);
  void statement(const Object &O);
  void envelope(const Object &O);
  void subject(const Object &O);
  void predicate(const Object &O);
  void times();

public:
  PackageAttestation read(const Artifact &Input,
                          const PackageEvidenceDescriptor &Descriptor,
                          std::string_view Kind);
};
} // namespace neverd::web::package_attestations

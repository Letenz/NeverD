//===- Admission.h - Shared offline metadata resource admission ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bound captured metadata before parsing, hashing and retaining evidence.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "../../JsonReader.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"
#include "neverd/web/PackageEvidence.h"

#include <optional>

namespace neverd::web::package_evidence {

inline constexpr uint64_t MaxInputBytes = MaxPackageEvidenceInputBytes;
inline constexpr uint64_t MaxWork = MaxPackageEvidenceSteps;
inline constexpr uint64_t MaxRetainedBytes = MaxPackageEvidenceRetainedBytes;

struct Budget {
  uint64_t Steps = 0;
  void charge(uint64_t Amount = 1) {
    if (Amount > MaxWork - Steps)
      throw Error("package_metadata_work_budget_exceeded");
    Steps += Amount;
  }
};

inline void requireIdentity(std::string_view ID) {
  if (ID.size() != 64 || ID.find_first_not_of("0123456789abcdef") != ID.npos)
    throw Error("package_metadata_invalid_identity");
}

inline llvm::json::Value document(const Artifact &Input, Budget &Work) {
  requireIdentity(Input.ID);
  requireIdentity(Input.BlobHash);
  if (Input.Directory || Input.Content.size() > MaxInputBytes)
    throw Error("package_metadata_input_budget_exceeded");
  Work.charge(Input.Content.size() * 2);
  const auto Bytes = Input.Content.read(0, Input.Content.size(), MaxInputBytes);
  if (sha256(Bytes) != Input.BlobHash)
    throw Error("package_metadata_hash_mismatch");
  uint64_t Nodes = 0;
  auto V = parseBoundedJSON(Bytes, {MaxInputBytes, 32, 32768, MaxInputBytes},
                            &Nodes);
  Work.charge(Nodes * 16);
  if (!V.getAsObject())
    throw Error("package_metadata_not_object");
  return V;
}

inline std::optional<std::string>
text(const llvm::json::Object &O, llvm::StringRef Key,
     size_t Limit = ::neverd::limits::web::kAdmissionTextLimit) {
  const auto *V = O.get(Key);
  if (!V)
    return {};
  const auto S = V->getAsString();
  if (!S || S->empty() || S->size() > Limit)
    throw Error("package_metadata_invalid_text");
  return S->str();
}

inline const llvm::json::Array *array(const llvm::json::Object &O,
                                      llvm::StringRef Key) {
  const auto *V = O.get(Key);
  if (!V)
    return nullptr;
  const auto *A = V->getAsArray();
  if (!A)
    throw Error("package_metadata_invalid_array");
  return A;
}
} // namespace neverd::web::package_evidence

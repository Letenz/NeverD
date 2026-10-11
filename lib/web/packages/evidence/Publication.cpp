//===- Publication.cpp - Metadata descriptor and time publication -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish admitted times and original anchors without source or trust claims.
///
//===----------------------------------------------------------------------===//

#include "Publication.h"

namespace neverd::web::package_evidence {
llvm::json::Object time(const PackageEvidenceTime &T) {
  return llvm::json::Object{{"status", T.Status},
                            {"value", T.Status == "valid_utc_timestamp"
                                          ? llvm::json::Value(T.Value)
                                          : llvm::json::Value(nullptr)}};
}
llvm::json::Object descriptor(const PackageEvidenceDescriptor &D) {
  return llvm::json::Object{{"descriptor_id", D.ID},
                            {"artifact_id", D.ArtifactID},
                            {"blob_sha256", D.BlobHash},
                            {"profile", std::string(PackageEvidenceProfile)},
                            {"validation", D.Status},
                            {"source_present", !D.Source.empty()},
                            {"source_label_redacted", true},
                            {"retrieved_at", time(D.RetrievedAt)},
                            {"as_of", time(D.AsOf)}};
}
} // namespace neverd::web::package_evidence

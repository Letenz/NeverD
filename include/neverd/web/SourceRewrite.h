//===- SourceRewrite.h - Reversible local source rewrites ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local evaluation receipts, unchanged originals and exact inverse checks.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Source.h"

namespace neverd::web {

inline constexpr std::string_view SourceRewriteProfile =
    "javascript-primitive-rewrite-v1";
inline constexpr std::string_view SourceRenameProfile =
    "javascript-scope-rename-v1";

/// One authority for pass admission, identities and publication profiles.
inline constexpr std::string_view sourceRewriteProfile(std::string_view Pass) {
  if (Pass == "string_recovery" || Pass == "constant_folding")
    return SourceRewriteProfile;
  if (Pass == "scope_renaming")
    return SourceRenameProfile;
  return {};
}
inline constexpr uint64_t MaxSourceRewriteRecords = 4096;
inline constexpr uint64_t MaxSourceRewriteBytes = MaxJavaScriptBytes;
inline constexpr uint64_t MaxSourceRewriteSteps = 8000000;
inline constexpr uint64_t MaxCachedSourceRewrites = 2;

struct SourceRewriteRecord {
  std::string ID, NodeID, Status, Reason, ValueKind;
  uint32_t Node = UINT32_MAX;
  uint64_t SourceStart = 0, SourceEnd = 0, Start = 0, End = 0;
  // Private replacement and inverse material. Ordinary metadata must not
  // publish these bytes or individual hashes of short recovered literals.
  std::string Before, After;
  // Lexical renaming evidence only; omitted from primitive-v1 receipts.
  std::string BindingID, Role;
};

struct SourceRewrite {
  std::string ID, SourceID, ArtifactID, SourceType, SourceHash, CandidateHash;
  std::string BindingID, ValueID, EffectID, Pass;
  std::string Status = "unavailable", Reason;
  std::vector<SourceRewriteRecord> Records;
  uint64_t SourceSize = 0, Accepted = 0, Refused = 0, Steps = 0;
  // A private, derived artifact. Never overwrite the captured input or
  // automatically expose this content through an ordinary metadata API.
  std::string Text;
};

/// Parse and analyze the supplied original using the shared C++ frontend.
/// Pass is string_recovery, constant_folding or scope_renaming. A substitution
/// does not preserve source reflection or prove whole-program equivalence.
SourceRewrite rewriteJavaScript(std::string_view ArtifactID,
                                std::string_view Bytes,
                                std::string_view SourceType,
                                std::string_view Pass);

/// Verify the exact candidate and receipt before reconstructing its original.
/// A receipt is local evidence, not a signed assertion from a trusted producer.
std::string undoSourceRewrite(const SourceRewrite &Rewrite,
                              std::string_view Candidate);

} // namespace neverd::web

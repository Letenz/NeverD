//===- LibraryRecognition.h - Read-only library evidence -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_LIBRARYRECOGNITION_H
#define NEVERD_SIGS_LIBRARYRECOGNITION_H

#include "neverd/sigs/LibraryFeature.h"

#include <tuple>

namespace neverd::sigs {

struct LibraryOccurrence {
  va_t Address = 0;
  int Sequence = -1;
  bool operator==(const LibraryOccurrence &) const = default;
  bool operator<(const LibraryOccurrence &Other) const {
    return std::tie(Address, Sequence) <
           std::tie(Other.Address, Other.Sequence);
  }
};

/// A presentation annotation, never a semantic declaration or a new callee.
/// Source version fields describe the rule's provenance, not the target's
/// exact library version. Emitters must independently prove output spans.
struct LibraryRecognition {
  va_t Function = 0;
  std::string Pack;
  std::string PackSHA256;
  std::string ProfileSHA256;
  std::string EvidenceSHA256;
  std::string Rule;
  unsigned RuleRevision = 0;
  std::string Family;
  std::string Operation;
  std::string ReceiverType;
  std::string DisplayName;
  std::string LinkageName;
  std::string IdentityEvidence;
  std::string SourceOrigin;
  std::string SourceRevision;
  LibraryFeatureScope Scope = LibraryFeatureScope::InlineExpression;
  std::vector<LibraryOccurrence> Occurrences;
  /// Exact result-producing occurrence for expression projection.
  std::optional<LibraryOccurrence> ResultOccurrence;
  /// Structural region isolation is separate from final source-map coverage.
  bool Isolated = false;
};

} // namespace neverd::sigs

#endif

//===- LibraryPresentation.h - Shared recognition JSON ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LIBRARYPRESENTATION_H
#define NEVERD_SDK_CAPI_LIBRARYPRESENTATION_H

#include "JSONText.h"

#include "neverd/sigs/LibraryRecognition.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd::sdk {

inline const char *nameOriginText(NameOrigin Origin) {
  switch (Origin) {
  case NameOrigin::User:
    return "user";
  case NameOrigin::Stated:
    return "stated";
  case NameOrigin::Analysis:
    return "analysis";
  case NameOrigin::Synthesized:
    return "synthesized";
  }
  llvm_unreachable("unknown name origin");
}

inline llvm::json::Object
libraryRecognitionJSON(const sigs::LibraryRecognition &Match) {
  const char *Scope =
      Match.Scope == sigs::LibraryFeatureScope::WholeFunction ? "whole-function"
      : Match.Scope == sigs::LibraryFeatureScope::InlineRegion ? "inline-region"
      : Match.Scope == sigs::LibraryFeatureScope::CallSite
          ? "call-site"
          : "inline-expression";
  llvm::json::Object Item{{"scope", Scope},
                          {"family", Match.Family},
                          {"operation", Match.Operation},
                          {"display_name", jsonSafeText(Match.DisplayName)},
                          {"linkage_name", jsonSafeText(Match.LinkageName)},
                          {"receiver_type", Match.ReceiverType},
                          {"identity_evidence", Match.IdentityEvidence},
                          {"pack_id", Match.Pack},
                          {"pack_sha256", Match.PackSHA256},
                          {"profile_sha256", Match.ProfileSHA256},
                          {"evidence_sha256", Match.EvidenceSHA256},
                          {"rule_id", Match.Rule},
                          {"rule_revision", Match.RuleRevision},
                          {"source_origin", Match.SourceOrigin},
                          {"source_revision", Match.SourceRevision},
                          {"isolated", Match.Isolated},
                          {"result_inverted", Match.ResultInverted},
                          {"result_is_condition", Match.ResultIsCondition}};
  if (Match.Callee)
    Item["callee"] = "0x" + llvm::utohexstr(*Match.Callee);
  llvm::json::Array Occurrences;
  for (const auto &Origin : Match.Occurrences)
    Occurrences.push_back(
        llvm::json::Object{{"address", "0x" + llvm::utohexstr(Origin.Address)},
                           {"origin_seq", Origin.Sequence}});
  Item["occurrences"] = std::move(Occurrences);
  return Item;
}
} // namespace neverd::sdk

#endif

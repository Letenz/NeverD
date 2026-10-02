#ifndef NEVERD_SDK_CAPI_OBJC_NATIVE_SOURCE_CALL_CALLEES_H
#define NEVERD_SDK_CAPI_OBJC_NATIVE_SOURCE_CALL_CALLEES_H

#include "neverd/ir/SourceABI.h"
#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

namespace neverd::sdk {
/// Current source-bound native declarations whose complete caller inventory,
/// callee HighIR and accepted audit agree. Dependency closure remains a
/// separate publication gate; this supplies only physical call contracts.
class NativePublicationCalleeIndex {
  std::map<va_t, const HighFunc *> HighCallees;
  std::map<va_t, const PipelineFunctionAudit *> CalleeAudits;

public:
  explicit NativePublicationCalleeIndex(const PipelineResult &Result) {
    for (const auto &Candidate : Result.HighFuncs) {
      const auto [It, Inserted] =
          HighCallees.emplace(Candidate.Entry, &Candidate);
      if (!Inserted)
        It->second = nullptr;
    }
    for (const auto &Candidate : Result.FunctionAudits) {
      const auto [It, Inserted] =
          CalleeAudits.emplace(Candidate.Entry, &Candidate);
      if (!Inserted)
        It->second = nullptr;
    }
  }

  std::map<va_t, SourceFunctionTypeHint>
  forCaller(const MedFunc &Caller) const {
    auto NativeCallees = boundNativeBooleanCallees(Caller);
    for (auto It = NativeCallees.begin(); It != NativeCallees.end();) {
      const auto High = HighCallees.find(It->first);
      const auto Audit = CalleeAudits.find(It->first);
      const bool Complete =
          High != HighCallees.end() && High->second &&
          High->second->SourceTypeHint &&
          equalSourceABIs(*High->second->SourceTypeHint, It->second) &&
          Audit != CalleeAudits.end() && Audit->second &&
          Audit->second->Disposition == PipelineFunctionDisposition::Accepted &&
          Audit->second->HasLowIR && Audit->second->HasMedIR &&
          Audit->second->MedIRVerified && Audit->second->DecodedInstructions &&
          Audit->second->DecodedInstructions ==
              Audit->second->LiftedInstructions &&
          Audit->second->DecodeFailures.empty() &&
          Audit->second->UnsupportedInstructions.empty() &&
          Audit->second->TruncatedPaths.empty();
      if (Complete)
        ++It;
      else
        It = NativeCallees.erase(It);
    }
    return NativeCallees;
  }
};

inline std::map<va_t, SourceFunctionTypeHint>
nativePublicationCallees(const PipelineResult &Result, const MedFunc &Caller) {
  return NativePublicationCalleeIndex(Result).forCaller(Caller);
}

} // namespace neverd::sdk
#endif

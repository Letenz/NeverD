#ifndef NEVERD_SDK_CAPI_OBJC_NATIVE_CURRENT_FUNCTION_H
#define NEVERD_SDK_CAPI_OBJC_NATIVE_CURRENT_FUNCTION_H

#include "neverd/ir/SourceABI.h"
#include "neverd/pipeline/Pipeline.h"

namespace neverd::sdk {
namespace native_source_detail {
struct CurrentFunction {
  const LowFunc *Low = nullptr;
  const MedFunc *Med = nullptr;
  const HighFunc *High = nullptr;
  const PipelineFunctionAudit *Audit = nullptr;
};

inline std::optional<CurrentFunction>
currentFunction(const PipelineResult &Result, va_t Entry,
                bool RequireReturningBody = true) {
  CurrentFunction Found;
  for (const auto &Candidate : Result.LowFuncs)
    if (Candidate.Entry == Entry) {
      if (Found.Low)
        return std::nullopt;
      Found.Low = &Candidate;
    }
  for (const auto &Candidate : Result.MedFuncs)
    if (Candidate.Entry == Entry) {
      if (Found.Med)
        return std::nullopt;
      Found.Med = &Candidate;
    }
  for (const auto &Candidate : Result.HighFuncs)
    if (Candidate.Entry == Entry) {
      if (Found.High)
        return std::nullopt;
      Found.High = &Candidate;
    }
  for (const auto &Candidate : Result.FunctionAudits)
    if (Candidate.Entry == Entry) {
      if (Found.Audit)
        return std::nullopt;
      Found.Audit = &Candidate;
    }
  if (!Found.Low || !Found.Med || !Found.High || !Found.Audit ||
      !Found.Low->hasCompleteLiftCoverage() || !Found.Med->SourceTypeHint ||
      !Found.High->SourceTypeHint || !Found.Med->SourceParametersBound ||
      (RequireReturningBody &&
       (Found.Med->DoesNotReturn || Found.High->DoesNotReturn)) ||
      Found.High->StructuredExceptionRegions ||
      Found.High->UnstructuredExceptionRegions ||
      !equalSourceABIs(*Found.Med->SourceTypeHint,
                       *Found.High->SourceTypeHint) ||
      Found.Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Found.Audit->HasLowIR || !Found.Audit->HasMedIR ||
      !Found.Audit->MedIRVerified || !Found.Audit->DecodedInstructions ||
      Found.Audit->DecodedInstructions != Found.Low->DecodedInstructionCount ||
      Found.Audit->LiftedInstructions != Found.Low->LiftedInstructionCount ||
      Found.Audit->DecodedInstructions != Found.Audit->LiftedInstructions ||
      !Found.Audit->DecodeFailures.empty() ||
      !Found.Audit->UnsupportedInstructions.empty() ||
      !Found.Audit->TruncatedPaths.empty())
    return std::nullopt;
  return Found;
}
} // namespace native_source_detail

} // namespace neverd::sdk
#endif

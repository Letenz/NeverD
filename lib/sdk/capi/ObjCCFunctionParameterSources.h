#ifndef NEVERD_SDK_CAPI_OBJCCFUNCTIONPARAMETERSOURCES_H
#define NEVERD_SDK_CAPI_OBJCCFUNCTIONPARAMETERSOURCES_H

#include "ObjCNativeSourceCallCallees.h"

#include "neverd/loader/MachO/CFunctionParameterCalls.h"

namespace neverd::sdk {
inline bool objCCFunctionParameterSourceCallBound(const HighExpr &Expression,
                                                  const BinaryImage &Image,
                                                  const PipelineResult &Result,
                                                  const HighFunc &Function) {
  if (!isCFunctionParameterSourceCall(Expression, Function, Image.Arch) ||
      !Result.Success || Result.SourceImage != &Image ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return false;
  const LowFunc *Low = nullptr;
  const MedFunc *Med = nullptr;
  const PipelineFunctionAudit *Audit = nullptr;
  const HighFunc *High = nullptr;
  for (const auto &Candidate : Result.LowFuncs)
    if (Candidate.Entry == Function.Entry) {
      if (Low)
        return false;
      Low = &Candidate;
    }
  for (const auto &Candidate : Result.MedFuncs)
    if (Candidate.Entry == Function.Entry) {
      if (Med)
        return false;
      Med = &Candidate;
    }
  for (const auto &Candidate : Result.HighFuncs)
    if (Candidate.Entry == Function.Entry) {
      if (High)
        return false;
      High = &Candidate;
    }
  for (const auto &Candidate : Result.FunctionAudits)
    if (Candidate.Entry == Function.Entry) {
      if (Audit)
        return false;
      Audit = &Candidate;
    }
  if (!Low || !Med || !High || !Audit || !Med->SourceTypeHint ||
      !High->SourceTypeHint || !Med->SourceParametersBound ||
      !equalSourceABIs(*Med->SourceTypeHint, *Function.SourceTypeHint) ||
      !equalSourceABIs(*High->SourceTypeHint, *Function.SourceTypeHint) ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      !Audit->DecodedInstructions ||
      Audit->DecodedInstructions != Low->DecodedInstructionCount ||
      Audit->LiftedInstructions != Low->LiftedInstructionCount ||
      Audit->DecodedInstructions != Audit->LiftedInstructions ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() || !Audit->TruncatedPaths.empty())
    return false;
  const auto Callees = nativePublicationCallees(Result, *Med);
  const auto Current = buildCFunctionParameterCallHints(
      Image, *Low, *Function.SourceTypeHint, &Callees);
  const auto &Evidence = *Expression.SourceCallHint->FunctionParameterCall;
  const auto Selected = Current.find(Evidence.Site.Instruction);
  if (Selected == Current.end() ||
      Selected->second.FunctionParameterCall != Evidence ||
      !equalSourceABIs(Selected->second.Signature,
                       Expression.SourceCallHint->Signature))
    return false;
  // Count source evaluations, including shared expression nodes. One saved
  // machine occurrence cannot justify two callback invocations.
  size_t Budget = 100000, Matches = 0;
  bool Foreign = false;
  std::set<SourceCallOccurrenceKey> Seen;
  std::vector<const HighStmt *> Statements;
  auto Append = [&](const std::vector<HighStmt> &Body) {
    if (Body.size() > Budget - std::min(Budget, Statements.size())) {
      Budget = 0;
      return;
    }
    for (const auto &Statement : Body)
      Statements.push_back(&Statement);
  };
  Append(Function.Body);
  while (!Statements.empty() && Budget) {
    --Budget;
    const auto &Statement = *Statements.back();
    Statements.pop_back();
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty() && Budget) {
        --Budget;
        auto Call = Pending.back();
        Pending.pop_back();
        if (!Call)
          continue;
        if (Call->SourceCallHint &&
            Call->SourceCallHint->FunctionParameterCall) {
          Matches += Call.get() == &Expression;
          const auto &Binding = *Call->SourceCallHint;
          const auto &Site = Binding.FunctionParameterCall->Site;
          const auto Found = Current.find(Site.Instruction);
          Foreign |=
              !isCFunctionParameterSourceCall(*Call, Function, Image.Arch) ||
              Found == Current.end() ||
              (Found != Current.end() &&
               (Found->second.FunctionParameterCall !=
                    Binding.FunctionParameterCall ||
                !equalSourceABIs(Found->second.Signature,
                                 Binding.Signature))) ||
              !Seen.insert(Site).second;
        }
        Call->forEachChildExpr([&](const ExprPtr &Child) {
          if (Pending.size() >= Budget)
            Budget = 0;
          else
            Pending.push_back(Child);
        });
      }
    });
    Append(Statement.Body);
    Append(Statement.ElseBody);
    for (const auto &Case : Statement.Cases)
      Append(Case.Body);
    Append(Statement.DefaultBody);
    for (const auto &Body : Statement.EHClauseBodies)
      Append(Body);
  }
  return Budget && !Foreign && Matches == 1;
}
} // namespace neverd::sdk
#endif

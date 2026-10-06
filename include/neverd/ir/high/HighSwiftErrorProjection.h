#ifndef NEVERD_IR_HIGH_HIGHSWIFTERRORPROJECTION_H
#define NEVERD_IR_HIGH_HIGHSWIFTERRORPROJECTION_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd {

inline bool isSwiftErrorEntrySlot(const HighFunc &Function,
                                  const ExprPtr &Expression) {
  if (!Function.SourceTypeHint || !Function.SwiftErrorEntry)
    return false;
  const auto Error = sourceABIErrorResult(*Function.SourceTypeHint);
  return Error && Expression && Expression->Kind == ExprKind::Var &&
         Expression->Var.Kind == MedVar::Param &&
         Expression->Var.Id == static_cast<int>(Error->ParameterIndex) &&
         Expression->Var.Size == 8 &&
         Expression->Var.RegOff == Error->Location.RegisterOffset &&
         equalSourceTypes(
             Expression->Type,
             Function.SourceTypeHint->Parameters[Error->ParameterIndex].Type);
}

/// Revalidate the source-only transport proof after HighIR transformations.
/// A declaration alone does not authorize dropping Swift's error output.
inline bool isSwiftErrorEntryProjected(const HighFunc &Function,
                                       Arch Architecture) {
  if (!Function.SourceTypeHint || !Function.SwiftErrorEntry ||
      Function.SourceTypeHint->Architecture != Architecture ||
      !equalSourceABIs(*Function.SourceTypeHint,
                       Function.SwiftErrorEntry->Signature) ||
      hasIndirectSourceParameters(*Function.SourceTypeHint))
    return false;
  const auto Error = sourceABIErrorResult(*Function.SourceTypeHint);
  if (!Error ||
      Function.Params.size() != Function.SourceTypeHint->Parameters.size() ||
      !equalSourceTypes(Function.ReturnType,
                        Function.SourceTypeHint->ReturnType))
    return false;
  for (size_t I = 0; I < Function.Params.size(); ++I)
    if (!equalSourceTypes(Function.Params[I].Type,
                          Function.SourceTypeHint->Parameters[I].Type))
      return false;
  const auto &Input = Function.SwiftErrorEntry->Input;
  if (Input.Kind != MedVar::Temp || Input.Id < 0 || Input.Size != 8 ||
      Input.TheArch != Architecture || Function.Body.empty())
    return false;
  const auto IsSlot = [&](const ExprPtr &Expression) {
    return isSwiftErrorEntrySlot(Function, Expression);
  };
  const auto &Capture = Function.Body.front();
  if (Capture.Kind != StmtKind::Assign || !Capture.Dst || !Capture.Val ||
      Capture.Dst->Kind != ExprKind::Var || Capture.Dst->Var != Input ||
      Capture.Val->Kind != ExprKind::Load ||
      Capture.Val->Operands.size() != 1 ||
      !IsSlot(Capture.Val->Operands.front()) ||
      !equalSourceTypes(Capture.Val->Type, Error->Type) ||
      Capture.Val->MemoryOrdering != NdMemoryOrdering::None ||
      Capture.Val->MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  size_t Budget = 100000;
  std::vector<std::pair<const std::vector<HighStmt> *, unsigned>> Lists{
      {&Function.Body, 0}};
  while (!Lists.empty()) {
    const auto [Body, Depth] = Lists.back();
    Lists.pop_back();
    if (Depth > 200 || Body->size() > Budget)
      return false;
    Budget -= Body->size();
    for (size_t I = 0; I < Body->size(); ++I) {
      const auto &Statement = (*Body)[I];
      if (Statement.Kind == StmtKind::Return) {
        if (!I)
          return false;
        const auto &Store = (*Body)[I - 1];
        if (Store.Kind != StmtKind::Store || Store.Addr != Statement.Addr ||
            Store.MemoryOrdering != NdMemoryOrdering::None ||
            Store.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            !IsSlot(Store.StoreAddr) || !Store.StoreVal ||
            !Store.StoreVal->Type || Store.StoreVal->Type->Size != 8 ||
            (Store.StoreVal->Type->Kind != NdTypeKind::Ptr &&
             Store.StoreVal->Type->Kind != NdTypeKind::Int))
          return false;
      }
      for (const auto *Nested :
           {&Statement.Body, &Statement.ElseBody, &Statement.DefaultBody})
        if (!Nested->empty())
          Lists.emplace_back(Nested, Depth + 1);
      for (const auto &Case : Statement.Cases)
        Lists.emplace_back(&Case.Body, Depth + 1);
      for (const auto &Clause : Statement.EHClauseBodies)
        Lists.emplace_back(&Clause, Depth + 1);
    }
  }
  return true;
}

} // namespace neverd
#endif

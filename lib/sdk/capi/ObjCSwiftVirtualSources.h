#ifndef NEVERD_SDK_CAPI_OBJCSWIFTVIRTUALSOURCES_H
#define NEVERD_SDK_CAPI_OBJCSWIFTVIRTUALSOURCES_H

#include "../../loader/Swift/SwiftMangledClassMethodABI.h"
#include "ObjCNativeCurrentFunction.h"
#include "ObjCSourceBindings.h"

#include "neverd/loader/Swift/SwiftVirtualCalls.h"

namespace neverd::sdk {
inline bool objCSwiftVirtualSourceCallBound(const HighExpr &Expression,
                                            const BinaryImage &Image,
                                            const PipelineResult &Result,
                                            const HighFunc &Function) {
  const auto IsNative = [](const SourceCallTypeHint &Binding) {
    return Binding.Virtual && Binding.Virtual->NativeSelfClass;
  };
  if (!Expression.SourceCallHint || !IsNative(*Expression.SourceCallHint) ||
      !Result.Success || Result.SourceImage != &Image ||
      !Function.SourceTypeHint || Function.DoesNotReturn ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions)
    return false;
  const auto Caller =
      native_source_detail::currentFunction(Result, Function.Entry);
  const auto Declaration =
      swiftMangledObjCObjectPairVoidMethodSourceABI(Image, Function.Entry);
  if (!Caller || !Declaration ||
      !equalSourceABIs(*Declaration, *Function.SourceTypeHint) ||
      !equalSourceABIs(*Caller->High->SourceTypeHint, *Declaration) ||
      Function.Params.size() != Declaration->Parameters.size())
    return false;
  for (size_t I = 0; I < Function.Params.size(); ++I)
    if (!equalSourceTypes(Function.Params[I].Type,
                          Declaration->Parameters[I].Type))
      return false;
  const auto Current = buildSwiftVirtualCallHints(Image, *Caller->Low);
  const auto MatchesCurrent = [&](const SourceCallTypeHint &Binding) {
    if (!IsNative(Binding) || !isSwiftVirtualSourceCallHint(Image, Binding))
      return false;
    const auto Found = Current.find(Binding.Virtual->CallSite);
    return Found != Current.end() && Found->second.Virtual == Binding.Virtual &&
           equalSourceABIs(Found->second.Signature, Binding.Signature);
  };
  std::map<va_t, MedVar> MedTargets;
  const auto ValidExpression = [&](const HighExpr &Call) {
    const auto Target =
        Call.SourceCallHint && Call.SourceCallHint->Virtual
            ? MedTargets.find(Call.SourceCallHint->Virtual->CallSite)
            : MedTargets.end();
    return Call.Kind == ExprKind::Call && Call.SourceCallHint &&
           MatchesCurrent(*Call.SourceCallHint) && Call.IsIndirectCall &&
           !Call.CallAddr && Call.IntrinsicId == Intrinsic::None &&
           Call.MemoryOrdering == NdMemoryOrdering::None &&
           Call.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           Call.IndirectTarget && Call.IndirectTarget->Kind == ExprKind::Var &&
           Call.IndirectTarget->Type && Call.IndirectTarget->Type->Size == 8 &&
           Target != MedTargets.end() &&
           Call.IndirectTarget->Var == Target->second &&
           Call.IndirectTarget->Var.Size == 8 && Call.Type &&
           Call.Type->Kind == NdTypeKind::Void && Call.Operands.size() == 1 &&
           Call.Operands[0] && Call.Operands[0]->Type &&
           Call.Operands[0]->Type->Size == 8 &&
           (Call.Operands[0]->Type->Kind == NdTypeKind::Ptr ||
            Call.Operands[0]->Type->Kind == NdTypeKind::Int) &&
           objc_binding_detail::exactParameterValue(Call.Operands[0], 2);
  };
  std::map<va_t, int> LowSites;
  for (const auto &Block : Caller->Low->Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INDIR_CALL && Current.count(Op.Addr))
        if (!LowSites.emplace(Op.Addr, Op.Seq).second)
          return false;
  std::set<va_t> MedSites, Seen;
  size_t Budget = 100000, Matches = 0;
  for (const auto &Block : Caller->Med->Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Budget--)
        return false;
      if (!Op.SourceCallHint || !IsNative(*Op.SourceCallHint))
        continue;
      const auto Site = LowSites.find(Op.Addr);
      if (!MatchesCurrent(*Op.SourceCallHint) ||
          Op.SourceCallHint->Virtual->CallSite != Op.Addr ||
          Op.Opcode != NdOp::INDIR_CALL || Op.NumInputs != 2 ||
          Op.Inputs[0].isConst() || Op.Inputs[0].Size != 8 ||
          Op.DoesNotReturn || Op.PreservesCallerSaved ||
          Site == LowSites.end() || Op.OriginSeq != Site->second ||
          !MedSites.insert(Op.Addr).second)
        return false;
      MedTargets.emplace(Op.Addr, Op.Inputs[0]);
    }
  if (MedSites.size() != LowSites.size() || !ValidExpression(Expression))
    return false;
  bool Invalid = false;
  std::vector<const HighStmt *> Statements;
  const auto Append = [&](const std::vector<HighStmt> &Body) {
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
        if (Call->SourceCallHint && IsNative(*Call->SourceCallHint)) {
          Matches += Call.get() == &Expression;
          Invalid |=
              !ValidExpression(*Call) ||
              !Seen.insert(Call->SourceCallHint->Virtual->CallSite).second;
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
  return Budget && !Invalid && Matches == 1 && Seen == MedSites;
}
} // namespace neverd::sdk
#endif

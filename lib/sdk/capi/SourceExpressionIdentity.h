#ifndef NEVERD_SDK_CAPI_SOURCEEXPRESSIONIDENTITY_H
#define NEVERD_SDK_CAPI_SOURCEEXPRESSIONIDENTITY_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"

namespace neverd::sdk {
// Bounded identity comparison for canonical source replays. This grants no
// semantics to a call binding; callers authenticate bindings separately.
inline bool sameSourceExpressionIdentity(const HighExpr &A, const HighExpr &B,
                                         size_t &Budget, unsigned Depth = 0) {
  if (!Budget || Depth == 128)
    return false;
  --Budget;
  if (A.Kind != B.Kind || A.Op != B.Op || !equalSourceTypes(A.Type, B.Type) ||
      A.MemoryOrdering != B.MemoryOrdering ||
      A.MemoryAddressSpace != B.MemoryAddressSpace ||
      A.Operands.size() != B.Operands.size() ||
      A.IntrinsicId != B.IntrinsicId ||
      A.IntrinsicOutputs != B.IntrinsicOutputs ||
      (A.Kind != ExprKind::Call && A.SourceCallHint != B.SourceCallHint) ||
      bool(A.IndirectTarget) != bool(B.IndirectTarget))
    return false;
  switch (A.Kind) {
  case ExprKind::Var:
    if (A.Var != B.Var || A.Var.Size != B.Var.Size ||
        A.Var.TheArch != B.Var.TheArch)
      return false;
    break;
  case ExprKind::Const:
    if (A.ConstVal != B.ConstVal || A.ConstProvenance != B.ConstProvenance ||
        A.AddressOwnerVA != B.AddressOwnerVA)
      return false;
    break;
  case ExprKind::Call:
    if (A.CallAddr != B.CallAddr || A.CallTarget != B.CallTarget ||
        A.IsIndirectCall != B.IsIndirectCall ||
        A.IndirectParamIdx != B.IndirectParamIdx ||
        A.IntrinsicId != B.IntrinsicId ||
        A.IntrinsicOutputs != B.IntrinsicOutputs ||
        A.SourceCallHint != B.SourceCallHint)
      return false;
    break;
  case ExprKind::Cast:
    if (!equalSourceTypes(A.CastTo, B.CastTo))
      return false;
    break;
  case ExprKind::Field:
    if (A.ConstVal != B.ConstVal)
      return false;
    break;
  case ExprKind::Undef:
    return false;
  default:
    break;
  }
  for (size_t I = 0; I < A.Operands.size(); ++I)
    if (!A.Operands[I] || !B.Operands[I] ||
        !sameSourceExpressionIdentity(*A.Operands[I], *B.Operands[I], Budget,
                                      Depth + 1))
      return false;
  return !A.IndirectTarget ||
         sameSourceExpressionIdentity(*A.IndirectTarget, *B.IndirectTarget,
                                      Budget, Depth + 1);
}

// Exact, bounded replay of a straight-line private-copy lifetime. The caller
// authenticates expression annotations; every initializer, memory effect and
// subsequent use remains in the comparison. Structured bodies need their own
// proof rather than being silently flattened.
template <class EqualExpression>
inline bool sameStraightLineSourceBody(const HighFunc &Expected,
                                       const HighFunc &Actual,
                                       EqualExpression Equal, size_t &Budget) {
  if (Expected.FrameSize != Actual.FrameSize ||
      Expected.FrameHeadroom != Actual.FrameHeadroom ||
      Expected.Body.size() != Actual.Body.size())
    return false;
  const auto Flat = [](const HighStmt &S) {
    return S.Body.empty() && S.ElseBody.empty() && S.Cases.empty() &&
           S.DefaultBody.empty() && S.EHClauses.empty() &&
           S.EHClauseBodies.empty();
  };
  for (size_t I = 0; I < Expected.Body.size(); ++I) {
    if (!Budget)
      return false;
    --Budget;
    const auto &A = Expected.Body[I], &B = Actual.Body[I];
    if (!Flat(A) || !Flat(B) || A.Kind != B.Kind || A.Addr != B.Addr ||
        A.MemoryOrdering != B.MemoryOrdering ||
        A.MemoryAddressSpace != B.MemoryAddressSpace ||
        A.GotoTarget != B.GotoTarget || A.LoopHeaderAddr != B.LoopHeaderAddr ||
        A.IsPhiCopy != B.IsPhiCopy)
      return false;
    const ExprPtr Left[] = {A.Dst,       A.Val,      A.Cond,     A.RetVal,
                            A.StoreAddr, A.StoreVal, A.CallExpr, A.SwitchExpr};
    const ExprPtr Right[] = {B.Dst,       B.Val,      B.Cond,     B.RetVal,
                             B.StoreAddr, B.StoreVal, B.CallExpr, B.SwitchExpr};
    for (size_t J = 0; J < std::size(Left); ++J)
      if (bool(Left[J]) != bool(Right[J]) ||
          (Left[J] && !Equal(Left[J], Right[J])))
        return false;
  }
  return true;
}

// Bounded structural identity for canonical source replays with loops. Every
// branch, case, backedge target and memory operation participates; exception
// regions require a separate proof and are deliberately unsupported here.
template <class EqualExpression>
inline bool sameStructuredSourceBody(const HighFunc &Expected,
                                     const HighFunc &Actual,
                                     EqualExpression Equal, size_t &Budget) {
  if (Expected.FrameSize != Actual.FrameSize ||
      Expected.FrameHeadroom != Actual.FrameHeadroom)
    return false;
  const auto Compare = [&](auto &&Self, const std::vector<HighStmt> &Left,
                           const std::vector<HighStmt> &Right,
                           unsigned Depth) -> bool {
    if (Depth >= 64 || Left.size() != Right.size() || Left.size() > Budget)
      return false;
    for (size_t I = 0; I < Left.size(); ++I) {
      if (!Budget)
        return false;
      --Budget;
      const auto &A = Left[I], &B = Right[I];
      if (A.Kind != B.Kind || A.Addr != B.Addr ||
          A.MemoryOrdering != B.MemoryOrdering ||
          A.MemoryAddressSpace != B.MemoryAddressSpace ||
          A.GotoTarget != B.GotoTarget ||
          A.LoopHeaderAddr != B.LoopHeaderAddr || A.IsPhiCopy != B.IsPhiCopy ||
          !A.EHClauses.empty() || !B.EHClauses.empty() ||
          !A.EHClauseBodies.empty() || !B.EHClauseBodies.empty() ||
          A.EHIsReducible || B.EHIsReducible ||
          A.EHRange.Begin != B.EHRange.Begin ||
          A.EHRange.End != B.EHRange.End || A.Cases.size() != B.Cases.size())
        return false;
      const ExprPtr X[] = {A.Dst,       A.Val,      A.Cond,     A.RetVal,
                           A.StoreAddr, A.StoreVal, A.CallExpr, A.SwitchExpr};
      const ExprPtr Y[] = {B.Dst,       B.Val,      B.Cond,     B.RetVal,
                           B.StoreAddr, B.StoreVal, B.CallExpr, B.SwitchExpr};
      for (size_t J = 0; J < std::size(X); ++J)
        if (bool(X[J]) != bool(Y[J]) || (X[J] && !Equal(X[J], Y[J])))
          return false;
      if (!Self(Self, A.Body, B.Body, Depth + 1) ||
          !Self(Self, A.ElseBody, B.ElseBody, Depth + 1) ||
          !Self(Self, A.DefaultBody, B.DefaultBody, Depth + 1))
        return false;
      for (size_t J = 0; J < A.Cases.size(); ++J)
        if (A.Cases[J].Value != B.Cases[J].Value ||
            A.Cases[J].FallsThrough != B.Cases[J].FallsThrough ||
            !Self(Self, A.Cases[J].Body, B.Cases[J].Body, Depth + 1))
          return false;
    }
    return true;
  };
  return Compare(Compare, Expected.Body, Actual.Body, 0);
}

} // namespace neverd::sdk
#endif

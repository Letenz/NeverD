//===- HighDeadStoreElim.cpp - Dead store elimination for HighIR ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead store elimination passes for HighIR:
///   - Consecutive dead store elimination (same-destination rewrites)
///   - Redundant stack store elimination (stores duplicated in call args)
///
/// See also:
///   HighDCE.cpp         — main DCE orchestration, iterative liveness DCE
///   HighCopyProp.cpp    — copy propagation and alias resolution
///   HighDCEDetail.h     — shared declarations
///
//===----------------------------------------------------------------------===//

#include "HighFrameAddress.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace neverd {

void elimUnreadPrivateFrameStores(HighFunc &Func, Arch Architecture) {
  if (Func.FrameSize <= 0 || Architecture == Arch::Unknown ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (TRI.PointerSize != 4 && TRI.PointerSize != 8)
    return;
  size_t Budget = 100000;

  // No frame read or escape is permitted anywhere in the function. This
  // includes arguments, stored pointers, returns and aliases. Ordinary calls
  // cannot observe newly allocated private source storage without its address.
  // Unknown address shapes and effects retain every store conservatively.
  std::vector<HighStmt *> Candidates;
  std::unordered_set<const HighExpr *> Seen;
  bool Observed = false;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (Observed || !Budget)
      return;
    --Budget;
    bool Candidate = false;
    if (S.Kind == StmtKind::Store && S.StoreVal && S.StoreVal->Type &&
        S.StoreVal->Type->Size && S.Body.empty() && S.ElseBody.empty() &&
        S.Cases.empty() && S.DefaultBody.empty() && S.EHClauseBodies.empty() &&
        (S.StoreVal->Kind == ExprKind::Var ||
         S.StoreVal->Kind == ExprKind::Const) &&
        S.MemoryOrdering == NdMemoryOrdering::None &&
        S.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      const auto At = high_detail::frameAddressOffset(S.StoreAddr, Func,
                                                      Architecture, Budget);
      Candidate = At && *At >= -Func.FrameSize && *At < 0 &&
                  uint64_t(S.StoreVal->Type->Size) <= uint64_t(-*At);
      if (Candidate)
        Candidates.push_back(&S);
    }
    forEachExpr(S, [&](const ExprPtr &Root) {
      if (Candidate && &Root == &S.StoreAddr)
        return;
      std::vector<const HighExpr *> Pending{Root.get()};
      while (!Pending.empty() && !Observed && Budget) {
        const auto *E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E).second)
          continue;
        --Budget;
        if (E->IntrinsicId != Intrinsic::None)
          Observed = true;
        if (E->Kind == ExprKind::Call) {
          std::string Error;
          if (!E->SourceCallHint ||
              !validateSourceABI(E->SourceCallHint->Signature, Error) ||
              E->Operands.size() !=
                  E->SourceCallHint->Signature.Parameters.size())
            Observed = true;
        }
        if (E->Kind == ExprKind::Var && (E->Var.Kind == MedVar::Stack ||
                                         (E->Var.Kind == MedVar::Reg &&
                                          (E->Var.RegOff == TRI.StackPointer ||
                                           E->Var.RegOff == TRI.FramePointer))))
          Observed = true;
        for (const auto &Operand : E->Operands)
          Pending.push_back(Operand.get());
      }
    });
  });
  if (Observed || !Budget)
    return;
  for (auto *S : Candidates) {
    // Preserve a possible branch destination at the eliminated write.
    const va_t Address = S->Addr;
    *S = HighStmt{};
    S->Kind = StmtKind::Block;
    S->Addr = Address;
  }
}

//===----------------------------------------------------------------------===//
// Consecutive dead store elimination
//===----------------------------------------------------------------------===//

void elimConsecutiveDeadStores(std::vector<HighStmt> &Stmts) {
  Stmts.erase(
      std::remove_if(Stmts.begin(), Stmts.end(),
                     [](const HighStmt &S) { return S.Kind == StmtKind::Nop; }),
      Stmts.end());

  for (size_t I = 0; I + 1 < Stmts.size(); ++I) {
    auto &CurrStmt = Stmts[I];
    auto &NextStmt = Stmts[I + 1];
    if (CurrStmt.Kind != StmtKind::Assign || NextStmt.Kind != StmtKind::Assign)
      continue;
    if (CurrStmt.Val && CurrStmt.Val->hasOrderedMemoryAccess())
      continue;
    if (!CurrStmt.Dst || !NextStmt.Dst)
      continue;
    if (CurrStmt.Dst->Kind != ExprKind::Var ||
        NextStmt.Dst->Kind != ExprKind::Var)
      continue;

    const auto &Current = CurrStmt.Dst->Var;
    const auto &Next = NextStmt.Dst->Var;
    // Physical registers are reused by distinct SSA values. Equal right-hand
    // sides do not make those destinations one mutable local, and a widening
    // conversion does not make the narrow and wide values interchangeable.
    const bool SameVariable =
        Current.Kind == Next.Kind && Current.TheArch == Next.TheArch &&
        Current.Id == Next.Id && Current.SSAVer == Next.SSAVer &&
        Current.RenameTag == Next.RenameTag && Current.Size == Next.Size &&
        Current.RegOff == Next.RegOff;
    if (!SameVariable || !CurrStmt.Val || !NextStmt.Val)
      continue;

    bool NextUsesCurr = false;
    std::unordered_set<const HighExpr *> Seen;
    std::function<void(const ExprPtr &)> CheckRef = [&](const ExprPtr &E) {
      if (!E || NextUsesCurr || !Seen.insert(E.get()).second)
        return;
      if (E->Kind == ExprKind::Var && E->Var == CurrStmt.Dst->Var)
        NextUsesCurr = true;
      for (auto &Op : E->Operands)
        CheckRef(Op);
    };
    if (NextStmt.Val)
      CheckRef(NextStmt.Val);

    if (NextUsesCurr)
      continue;

    bool HasEffect = false;
    std::vector<const HighExpr *> Pending{CurrStmt.Val.get()};
    std::unordered_set<const HighExpr *> EffectSeen;
    while (!Pending.empty()) {
      const auto *Expression = Pending.back();
      Pending.pop_back();
      if (!Expression || !EffectSeen.insert(Expression).second)
        continue;
      HasEffect |= Expression->Kind == ExprKind::Call ||
                   Expression->Kind == ExprKind::Store;
      for (const auto &Operand : Expression->Operands)
        Pending.push_back(Operand.get());
    }
    if (CurrStmt.Val->Kind == ExprKind::Call) {
      CurrStmt.Kind = StmtKind::Call;
      CurrStmt.CallExpr = CurrStmt.Val;
      CurrStmt.Dst = nullptr;
      CurrStmt.Val = nullptr;
    } else if (HasEffect) {
      CurrStmt.Kind = StmtKind::ExprStmt;
      CurrStmt.Dst = nullptr;
    } else {
      Stmts.erase(Stmts.begin() + static_cast<long>(I));
      --I;
    }
  }

  for (auto &S : Stmts) {
    elimConsecutiveDeadStores(S.Body);
    elimConsecutiveDeadStores(S.ElseBody);
    for (auto &C : S.Cases)
      elimConsecutiveDeadStores(C.Body);
    elimConsecutiveDeadStores(S.DefaultBody);
  }
}

} // namespace neverd

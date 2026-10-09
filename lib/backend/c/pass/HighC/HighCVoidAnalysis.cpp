//===- HighCVoidAnalysis.cpp - Void return analysis -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Void-return inference and dead-chain propagation for the HighIR C emitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace neverd {

namespace {

// Store forwarding hides the original store and caches its rendered value.
// A surviving load therefore reads the cached value's dependencies as well as
// the variables visible in HighIR. Later liveness passes must use both views
// or they can delete the only definition of a name still present in C.
void collectRenderedUses(const HighCAnalysisState &State, const HighExpr &Expr,
                         std::map<std::string, TypeRef> &Used, VarNameFn VarFn,
                         CallArgLimitFn ArgLimit) {
  collectUsedVarsExpr(Expr, Used, VarFn, ArgLimit);
  if (State.ForwardedAddressDeps.empty())
    return;

  std::set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{&Expr};
  while (!Work.empty()) {
    const HighExpr *Current = Work.back();
    Work.pop_back();
    if (!Current || !Seen.insert(Current).second)
      continue;
    // Taking the address of a load does not read its forwarded value.
    if (Current->Kind == ExprKind::Addr && Current->Operands.size() == 1 &&
        Current->Operands[0] && Current->Operands[0]->Kind == ExprKind::Load) {
      for (const ExprPtr &Address : Current->Operands[0]->Operands)
        Work.push_back(Address.get());
      continue;
    }
    if (Current->Kind == ExprKind::Load && Current->Operands.size() == 1 &&
        Current->Operands[0] &&
        Current->MemoryOrdering == NdMemoryOrdering::None) {
      auto Key = State.AddressKeys.find(Current->Operands[0].get());
      if (Key != State.AddressKeys.end()) {
        auto Deps = State.ForwardedAddressDeps.find(Key->second);
        if (Deps != State.ForwardedAddressDeps.end())
          for (const std::string &Name : Deps->second)
            Used.try_emplace(Name);
      }
    }
    size_t Limit = Current->Operands.size();
    if (Current->Kind == ExprKind::Call && ArgLimit)
      Limit = std::min(Limit, ArgLimit(*Current));
    for (size_t I = 0; I < Limit; ++I)
      Work.push_back(Current->Operands[I].get());
    if (Current->IndirectTarget)
      Work.push_back(Current->IndirectTarget.get());
  }
}

} // namespace

bool isNoreturnCallExpr(const HighExpr &E) {
  // A bare return in the caller says nothing about whether another callee
  // returns. In particular, HighIR can assign a result to register version 0.
  // Only a known terminating operation authorizes omitting its result.
  return E.Kind == ExprKind::Call &&
         (E.DoesNotReturn || libc::isNoReturnFunction(E.CallTarget) ||
          isX86FastFailCall(E));
}

bool highCExpressionHasEffect(const HighExpr &E) {
  std::set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Pending{&E};
  while (!Pending.empty()) {
    const HighExpr *Current = Pending.back();
    Pending.pop_back();
    if (!Current || !Seen.insert(Current).second)
      continue;
    if (Current->Kind == ExprKind::Call || Current->Kind == ExprKind::Store ||
        Current->MemoryOrdering != NdMemoryOrdering::None)
      return true;
    Current->forEachChildExpr([&](const ExprPtr &Child) {
      if (Child)
        Pending.push_back(Child.get());
    });
  }
  return false;
}

bool analyzeVoidReturn(const HighFunc &Func) {
  if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
    return Func.SourceTypeHint->ReturnType->Kind == NdTypeKind::Void;
  if (Func.ReturnType && Func.ReturnType->Kind == NdTypeKind::Void)
    return true;
  // Whether the function returns a value is settled on MedIR, for both C
  // backends (settleReturnContracts).
  return Func.ReturnsNoValue;
}

void analyzeVoidDeadChain(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn) {
  std::set<std::string> NonReturnUses;
  std::set<const HighExpr *> SeenUses;
  std::function<void(const HighExpr &)> CollectUse = [&](const HighExpr &E) {
    if (!SeenUses.insert(&E).second)
      return;
    if (E.Kind == ExprKind::Var)
      NonReturnUses.insert(VarFn(E.Var));
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        CollectUse(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return || State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        CollectUse(*E);
    });
  });

  std::function<void(const HighExpr &)> Collect = [&](const HighExpr &E) {
    if (E.Kind == ExprKind::Var) {
      const std::string Name = VarFn(E.Var);
      if (!NonReturnUses.count(Name))
        State.DeadVars.insert(Name);
    }
    for (const ExprPtr &Op : E.Operands)
      if (Op)
        Collect(*Op);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return && S.RetVal)
      Collect(*S.RetVal);
  });

  // A void return reads nothing: a call whose result only reached one prints
  // as a plain call, without a declared destination.
  std::set<std::string> Read;
  std::function<void(const HighExpr &)> CollectRead = [&](const HighExpr &E) {
    if (isNamedValueExpr(E))
      Read.insert(VarFn(E.Var));
    E.forEachChildExpr([&](const ExprPtr &Child) {
      if (Child)
        CollectRead(*Child);
    });
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Return || State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        CollectRead(*E);
    });
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
        S.Val->Kind == ExprKind::Call && isNamedValueExpr(*S.Dst) &&
        !Read.count(VarFn(S.Dst->Var)))
      State.OmittedCallResults.insert(&S);
  });
}

void analyzeUnusedAssigns(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn, CallArgLimitFn ArgLimit) {
  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 8) {
    Changed = false;
    std::map<std::string, TypeRef> Used;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      forEachRhsExpr(S, [&](const ExprPtr &E) {
        if (E)
          collectRenderedUses(State, *E, Used, VarFn, ArgLimit);
      });
    });
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (State.DeadStmts.count(&S))
        return;
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      if (highCExpressionHasEffect(*S.Val))
        return;
      if (Used.count(VarFn(S.Dst->Var)))
        return;
      State.DeadStmts.insert(&S);
      Changed = true;
    });
  }
}

void analyzeUnusedCallResults(HighCAnalysisState &State, const HighFunc &Func,
                              VarNameFn VarFn, CallArgLimitFn ArgLimit) {
  std::map<std::string, TypeRef> Used;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S))
      return;
    forEachRhsExpr(S, [&](const ExprPtr &E) {
      if (E)
        collectRenderedUses(State, *E, Used, VarFn, ArgLimit);
    });
  });
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (State.DeadStmts.count(&S) || State.OmittedCallResults.count(&S))
      return;
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    if (S.Val->Kind != ExprKind::Call)
      return;
    if (isNoreturnCallExpr(*S.Val))
      return;
    if (Used.count(VarFn(S.Dst->Var)))
      return;
    State.OmittedCallResults.insert(&S);
  });

  // HighC prints cleanup `call();` and drops the trailing `return dest`.
  // The dest is still "used" by that skipped return, so omit it here.
  std::function<void(const std::vector<HighStmt> &)> WalkCleanup;
  WalkCleanup = [&](const std::vector<HighStmt> &Stmts) {
    for (const HighStmt &S : Stmts) {
      WalkCleanup(S.Body);
      WalkCleanup(S.ElseBody);
      for (const auto &C : S.Cases)
        WalkCleanup(C.Body);
      WalkCleanup(S.DefaultBody);
      for (size_t C = 0; C < S.EHClauseBodies.size(); ++C) {
        const bool Cleanup =
            C < S.EHClauses.size() &&
            S.EHClauses[C].Kind == HighEHClauseKind::CxxCleanup;
        if (Cleanup) {
          const auto &Body = S.EHClauseBodies[C];
          for (size_t J = 0; J < Body.size(); ++J) {
            const HighStmt &CS = Body[J];
            if (State.DeadStmts.count(&CS))
              continue;
            if (CS.Kind != StmtKind::Assign || !CS.Dst || !CS.Val)
              continue;
            if (CS.Val->Kind != ExprKind::Call)
              continue;
            const HighStmt *Ret = nullptr;
            for (size_t K = J + 1; K < Body.size(); ++K) {
              if (Body[K].Kind == StmtKind::Nop)
                continue;
              if (Body[K].Kind == StmtKind::Return)
                Ret = &Body[K];
              break;
            }
            if (Ret)
              State.OmittedCallResults.insert(&CS);
          }
        }
        WalkCleanup(S.EHClauseBodies[C]);
      }
    }
  };
  WalkCleanup(Func.Body);
}

} // namespace neverd

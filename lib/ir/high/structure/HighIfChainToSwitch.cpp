//===- HighIfChainToSwitch.cpp - If-chain to switch recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recovers switch statements from cascading if-chains that compare the
/// same variable against a series of constants.  Handles default case
/// detection, dead-code cleanup after fully-terminating switches, and
/// guard-before-switch merging.
///
/// See also:
///   HighCFSimplify.cpp         — main simplifyControlFlow entry point
///   HighLoopRecovery.cpp       — while-loop recovery
///   NdOpSwitchRecovery.cpp     — jump-table-based switch recovery (LowIR)
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/ir/high/MedToHigh.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace neverd {

void recoverSwitchStatements(HighFunc &Func) {
  for (size_t I = 0; I < Func.Body.size(); ++I) {
    if (Func.Body[I].Kind != StmtKind::If)
      continue;
    if (Func.Body[I].Body.size() != 1 ||
        Func.Body[I].Body[0].Kind != StmtKind::Goto)
      continue;
    if (!Func.Body[I].Cond)
      continue;
    if (Func.Body[I].Cond->hasOrderedMemoryAccess())
      continue;

    auto ExtractCaseVal = [](const ExprPtr &Cond)
        -> std::optional<std::pair<std::string, uint64_t>> {
      if (!Cond || Cond->Kind != ExprKind::BinOp || Cond->Op != NdOp::INT_EQUAL)
        return std::nullopt;
      auto &Ops = Cond->Operands;
      if (Ops.size() != 2)
        return std::nullopt;
      if (Ops[0]->Kind == ExprKind::BinOp && Ops[0]->Op == NdOp::INT_SUB &&
          Ops[1]->Kind == ExprKind::Const && Ops[1]->ConstVal == 0) {
        auto &SubOps = Ops[0]->Operands;
        if (SubOps.size() == 2 && SubOps[1]->Kind == ExprKind::Const) {
          std::string Key = SubOps[0]->str();
          return std::make_pair(Key, SubOps[1]->ConstVal);
        }
      }
      if (Ops[1]->Kind == ExprKind::Const) {
        std::string Key = Ops[0]->str();
        return std::make_pair(Key, Ops[1]->ConstVal);
      }
      return std::nullopt;
    };

    auto FirstCase = ExtractCaseVal(Func.Body[I].Cond);
    if (!FirstCase)
      continue;

    struct CaseInfo {
      uint64_t Value;
      va_t BodyAddr;
      size_t StmtIdx;
    };
    std::vector<CaseInfo> CaseInfos;
    CaseInfos.push_back(
        {FirstCase->second, Func.Body[I].Body[0].GotoTarget, I});

    size_t ChainEnd = I + 1;
    std::set<uint64_t> SeenValues;
    SeenValues.insert(FirstCase->second);
    for (size_t J = I + 1; J < Func.Body.size(); ++J) {
      auto &S = Func.Body[J];
      if (S.Kind != StmtKind::If || S.Body.size() != 1 ||
          S.Body[0].Kind != StmtKind::Goto)
        break;
      auto CaseVal = ExtractCaseVal(S.Cond);
      if (!CaseVal || S.Cond->hasOrderedMemoryAccess() ||
          CaseVal->first != FirstCase->first)
        break;
      if (SeenValues.count(CaseVal->second))
        break;
      SeenValues.insert(CaseVal->second);
      CaseInfos.push_back({CaseVal->second, S.Body[0].GotoTarget, J});
      ChainEnd = J + 1;
    }

    if (CaseInfos.size() < 3)
      continue;

    va_t DefaultTarget = 0;
    size_t DefaultGotoIdx = SIZE_MAX;
    if (ChainEnd < Func.Body.size() &&
        Func.Body[ChainEnd].Kind == StmtKind::Goto) {
      DefaultTarget = Func.Body[ChainEnd].GotoTarget;
      DefaultGotoIdx = ChainEnd;
      ChainEnd++;
    }

    HighStmt SwitchStmt;
    SwitchStmt.Kind = StmtKind::Switch;
    SwitchStmt.Addr = Func.Body[I].Addr;
    for (auto &Case : CaseInfos) {
      size_t Idx = Case.StmtIdx;
      auto &Cond = Func.Body[Idx].Cond;
      if (Cond && Cond->Kind == ExprKind::BinOp && Cond->Operands.size() == 2) {
        auto &LHS = Cond->Operands[0];
        if (LHS->Kind == ExprKind::BinOp && LHS->Op == NdOp::INT_SUB &&
            LHS->Operands.size() == 2) {
          SwitchStmt.SwitchExpr = LHS->Operands[0];
          break;
        }
        SwitchStmt.SwitchExpr = LHS;
      }
    }
    if (!SwitchStmt.SwitchExpr)
      SwitchStmt.SwitchExpr = HighExpr::makeConst(0, 4);

    // A case body moves out of the list whole: the block at the case target,
    // up to and including its jump or return. The block must start exactly at
    // the target and must not be entered by falling through, or moving it
    // would change what runs on that other path.
    struct CaseBlock {
      size_t Begin = 0;
      size_t End = 0;
    };
    auto FindCaseBlock = [&](va_t Target) -> std::optional<CaseBlock> {
      size_t K = 0;
      while (K < Func.Body.size() && (Func.Body[K].Addr != Target ||
                                      Func.Body[K].Kind == StmtKind::Nop))
        ++K;
      if (K == Func.Body.size())
        return std::nullopt;
      size_t Before = K;
      while (Before > 0 && (Func.Body[Before - 1].Kind == StmtKind::Nop ||
                            (Func.Body[Before - 1].Kind == StmtKind::Block &&
                             Func.Body[Before - 1].Body.empty())))
        --Before;
      if (Before == 0 || (Func.Body[Before - 1].Kind != StmtKind::Goto &&
                          Func.Body[Before - 1].Kind != StmtKind::Return))
        return std::nullopt;
      for (size_t M = K; M < Func.Body.size(); ++M)
        if (Func.Body[M].Kind == StmtKind::Goto ||
            Func.Body[M].Kind == StmtKind::Return) {
          // The chain itself cannot move into one of its cases.
          if (K < ChainEnd && M + 1 > I)
            return std::nullopt;
          return CaseBlock{K, M + 1};
        }
      return std::nullopt;
    };

    // Cases that branch to one target share one copy of its statements:
    // `case A: case B: body`.  Copying the body per case duplicated its
    // labels (FsRtlIsTotalDeviceFailure).
    std::map<va_t, size_t> FirstUse;
    for (size_t K = 0; K < CaseInfos.size(); ++K)
      FirstUse.try_emplace(CaseInfos[K].BodyAddr, K);
    std::stable_sort(CaseInfos.begin(), CaseInfos.end(),
                     [&](const CaseInfo &A, const CaseInfo &B) {
                       return FirstUse[A.BodyAddr] < FirstUse[B.BodyAddr];
                     });
    std::map<va_t, CaseBlock> Blocks;
    bool Movable = true;
    for (const CaseInfo &Case : CaseInfos)
      if (!Blocks.count(Case.BodyAddr)) {
        std::optional<CaseBlock> Block = FindCaseBlock(Case.BodyAddr);
        Movable &= Block.has_value();
        if (Block)
          Blocks.emplace(Case.BodyAddr, *Block);
      }
    std::optional<CaseBlock> DefaultBlock;
    if (Movable && DefaultTarget != 0) {
      DefaultBlock = FindCaseBlock(DefaultTarget);
      Movable = DefaultBlock.has_value();
    }
    if (!Movable)
      continue;

    std::set<size_t> Consumed;
    auto TakeBlock = [&](const CaseBlock &Block) {
      std::vector<HighStmt> Body(Func.Body.begin() + Block.Begin,
                                 Func.Body.begin() + Block.End);
      for (size_t M = Block.Begin; M < Block.End; ++M)
        Consumed.insert(M);
      return Body;
    };
    for (size_t CaseIdx = 0; CaseIdx < CaseInfos.size(); ++CaseIdx) {
      const CaseInfo &Case = CaseInfos[CaseIdx];
      SwitchCase NewCase;
      NewCase.Value = Case.Value;
      Consumed.insert(Case.StmtIdx);
      if (CaseIdx + 1 < CaseInfos.size() &&
          CaseInfos[CaseIdx + 1].BodyAddr == Case.BodyAddr) {
        NewCase.FallsThrough = true;
        SwitchStmt.Cases.push_back(std::move(NewCase));
        continue;
      }
      NewCase.Body = TakeBlock(Blocks.at(Case.BodyAddr));
      SwitchStmt.Cases.push_back(std::move(NewCase));
    }

    if (DefaultBlock) {
      SwitchStmt.DefaultBody = TakeBlock(*DefaultBlock);
      Consumed.insert(DefaultGotoIdx);
    }

    Func.Body[I] = std::move(SwitchStmt);
    std::vector<size_t> ToRemove(Consumed.begin(), Consumed.end());
    std::sort(ToRemove.rbegin(), ToRemove.rend());
    for (size_t Idx : ToRemove) {
      if (Idx != I && Idx < Func.Body.size())
        Func.Body.erase(Func.Body.begin() + static_cast<long>(Idx));
    }

    // A case that jumps to what follows the switch breaks out of it; any
    // other jump stays, since breaking would skip the code it jumps to.
    if (I + 1 < Func.Body.size() && Func.Body[I].Kind == StmtKind::Switch) {
      const va_t After = Func.Body[I + 1].Addr;
      auto DropBreakJump = [&](std::vector<HighStmt> &Body) {
        if (!Body.empty() && Body.back().Kind == StmtKind::Goto && After &&
            Body.back().GotoTarget == After)
          Body.pop_back();
      };
      for (SwitchCase &Case : Func.Body[I].Cases)
        DropBreakJump(Case.Body);
      DropBreakJump(Func.Body[I].DefaultBody);
    }

    if (I + 1 < Func.Body.size() && Func.Body[I].Kind == StmtKind::Switch) {
      auto &SwitchRef = Func.Body[I];
      if (switchAlwaysReturns(SwitchRef)) {
        // Code after the switch is dead only up to the next goto target.
        std::set<va_t> Targets;
        walkStmts(Func.Body, [&](const HighStmt &S) {
          if (S.Kind == StmtKind::Goto)
            Targets.insert(S.GotoTarget);
        });
        size_t J = I + 1;
        while (J < Func.Body.size()) {
          auto &Dead = Func.Body[J];
          if (Dead.Kind == StmtKind::While || Dead.Kind == StmtKind::Switch)
            break;
          bool Labeled = false;
          walkStmts(std::vector<HighStmt>{Dead}, [&](const HighStmt &S) {
            Labeled |= S.Addr != 0 && Targets.count(S.Addr);
          });
          if (Labeled)
            break;
          J++;
        }
        if (J > I + 1) {
          Func.Body.erase(Func.Body.begin() + static_cast<long>(I + 1),
                          Func.Body.begin() + static_cast<long>(J));
        }
      }
    }

    break;
  }
}

} // namespace neverd

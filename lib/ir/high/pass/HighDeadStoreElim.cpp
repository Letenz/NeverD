//===- HighDeadStoreElim.cpp - Dead store elimination for HighIR ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dead store elimination passes for HighIR:
///   - Consecutive dead store elimination (same-destination rewrites)
///   - Private frame byte liveness (unobserved stores and integer tails)
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
#include <set>
#include <unordered_set>

namespace neverd {

namespace {
// Only remove or slice values whose evaluation cannot read memory, trap or
// call another function. Unknown bits may be discarded only when their exact
// private bytes have no reads; this never supplies a replacement bit value.
bool discardableFrameValue(const ExprPtr &Root, size_t &Budget) {
  std::vector<const HighExpr *> Pending{Root.get()};
  std::unordered_set<const HighExpr *> Seen;
  while (!Pending.empty()) {
    const auto *E = Pending.back();
    Pending.pop_back();
    if (!E || !Budget)
      return false;
    if (!Seen.insert(E).second)
      continue;
    --Budget;
    if (!E->Type || !E->Type->Size || E->IntrinsicId != Intrinsic::None ||
        E->MemoryOrdering != NdMemoryOrdering::None ||
        E->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return false;
    switch (E->Kind) {
    case ExprKind::Var:
    case ExprKind::Const:
    case ExprKind::Undef:
      if (!E->Operands.empty())
        return false;
      break;
    case ExprKind::Cast:
    case ExprKind::UnaryOp:
      if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 1 ||
          !E->Operands[0] || !E->Operands[0]->Type ||
          E->Operands[0]->Type->Kind != NdTypeKind::Int)
        return false;
      if (E->Kind == ExprKind::UnaryOp &&
          ((E->Op != NdOp::INT_ZEXT && E->Op != NdOp::INT_SEXT) ||
           E->Type->Size < E->Operands[0]->Type->Size))
        return false;
      break;
    case ExprKind::BinOp:
      if (E->Type->Kind != NdTypeKind::Int || E->Operands.size() != 2 ||
          !E->Operands[0] || !E->Operands[0]->Type ||
          E->Operands[0]->Type->Kind != NdTypeKind::Int || !E->Operands[1] ||
          !E->Operands[1]->Type ||
          E->Operands[1]->Type->Kind != NdTypeKind::Int)
        return false;
      if (E->Op == NdOp::CONCAT) {
        if (E->Type->Size !=
            E->Operands[0]->Type->Size + E->Operands[1]->Type->Size)
          return false;
      } else if (E->Op == NdOp::SUBBYTES) {
        if (E->Operands[1]->Kind != ExprKind::Const ||
            E->Operands[1]->ConstVal > E->Operands[0]->Type->Size ||
            E->Type->Size >
                E->Operands[0]->Type->Size - E->Operands[1]->ConstVal)
          return false;
      } else {
        return false;
      }
      break;
    default:
      return false;
    }
    for (const auto &Operand : E->Operands)
      Pending.push_back(Operand.get());
  }
  return true;
}

ExprPtr frameValuePrefix(ExprPtr Value, uint16_t Bytes) {
  // CONCAT's low operand owns the low-address bytes in supported native IR.
  // Preserve its exact expression instead of retaining unknown high padding.
  if (Value->Kind == ExprKind::BinOp && Value->Op == NdOp::CONCAT &&
      Value->Operands.size() == 2 && Value->Operands[0] &&
      Value->Operands[0]->Type && Value->Operands[1] &&
      Value->Operands[1]->Type &&
      Value->Type->Size ==
          Value->Operands[0]->Type->Size + Value->Operands[1]->Type->Size &&
      Value->Operands[1]->Type->Size >= Bytes)
    Value = Value->Operands[1];
  if (Value->Type->Size == Bytes)
    return Value;
  auto Prefix =
      HighExpr::makeBinop(NdOp::SUBBYTES, Value, HighExpr::makeConst(0, 4));
  Prefix->Type = NdType::makeInt(Bytes, false);
  return Prefix;
}
} // namespace

void elimUnreadPrivateFrameStores(HighFunc &Func, Arch Architecture) {
  if (Func.FrameSize <= 0 || Architecture == Arch::Unknown ||
      Func.StructuredExceptionRegions || Func.UnstructuredExceptionRegions)
    return;
  const auto &TRI = getTargetRegInfo(Architecture);
  if (TRI.PointerSize != 4 && TRI.PointerSize != 8)
    return;
  size_t Budget = 100000;
  VarKeyMap<unsigned> Definitions;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (!Budget)
      return;
    --Budget;
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      ++Definitions[varKey(S.Dst->Var)];
  });
  // Only a contiguous entry prefix establishes aliases. Its single-definition
  // addresses dominate all later uses without assuming arbitrary HighIR SSA
  // definitions dominate their uses across branches or exceptional paths.
  VarKeyMap<ExprPtr> Aliases;
  std::unordered_set<const HighStmt *> AliasStatements;
  for (const auto &S : Func.Body) {
    if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
        S.Dst->Kind != ExprKind::Var || !S.Dst->Type ||
        S.Dst->Type->Size != TRI.PointerSize ||
        S.Dst->Var.Size != TRI.PointerSize ||
        S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !S.Body.empty() || !S.ElseBody.empty() || !S.Cases.empty() ||
        !S.DefaultBody.empty() || !S.EHClauseBodies.empty() ||
        Definitions[varKey(S.Dst->Var)] != 1 ||
        !high_detail::frameAddressOffset(S.Val, Func, Architecture, Budget, 0,
                                         &Aliases))
      break;
    Aliases.emplace(varKey(S.Dst->Var), S.Val);
    AliasStatements.insert(&S);
  }
  const auto Address = [&](const ExprPtr &E, uint16_t Bytes) {
    auto At = high_detail::frameAddressOffset(E, Func, Architecture, Budget, 0,
                                              &Aliases);
    if (!At || !Bytes || *At < -Func.FrameSize || *At >= 0 ||
        uint64_t(Bytes) > uint64_t(-*At))
      return std::optional<int64_t>{};
    return At;
  };
  struct Candidate {
    HighStmt *Statement;
    int64_t Offset;
    uint16_t Bytes;
  };
  std::vector<Candidate> Candidates;
  std::set<int64_t> ReadBytes;
  std::unordered_set<const HighExpr *> Seen;
  bool Escaped = false;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (Escaped || !Budget || AliasStatements.count(&S))
      return;
    --Budget;
    bool PrivateStore = false;
    if (S.Kind == StmtKind::Store && S.StoreVal && S.StoreVal->Type &&
        S.Body.empty() && S.ElseBody.empty() && S.Cases.empty() &&
        S.DefaultBody.empty() && S.EHClauseBodies.empty() &&
        S.MemoryOrdering == NdMemoryOrdering::None &&
        S.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (const auto At = Address(S.StoreAddr, S.StoreVal->Type->Size)) {
        PrivateStore = true;
        if (discardableFrameValue(S.StoreVal, Budget))
          Candidates.push_back({&S, *At, S.StoreVal->Type->Size});
      }
    }
    forEachExpr(S, [&](const ExprPtr &Root) {
      if (PrivateStore && &Root == &S.StoreAddr)
        return;
      std::vector<const HighExpr *> Pending{Root.get()};
      while (!Pending.empty() && !Escaped && Budget) {
        const auto *E = Pending.back();
        Pending.pop_back();
        if (!E || !Seen.insert(E).second)
          continue;
        --Budget;
        if (E->IntrinsicId != Intrinsic::None)
          Escaped = true;
        if (E->Kind == ExprKind::Load && E->Type && E->Operands.size() == 1 &&
            E->MemoryOrdering == NdMemoryOrdering::None &&
            E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          if (const auto At = Address(E->Operands[0], E->Type->Size)) {
            if (E->Type->Size > Budget) {
              Budget = 0;
              return;
            }
            Budget -= E->Type->Size;
            for (unsigned I = 0; I < E->Type->Size; ++I)
              ReadBytes.insert(*At + I);
            continue;
          }
        }
        if (E->Kind == ExprKind::Call) {
          std::string Error;
          if (!E->SourceCallHint ||
              !validateSourceABI(E->SourceCallHint->Signature, Error) ||
              E->Operands.size() !=
                  E->SourceCallHint->Signature.Parameters.size())
            Escaped = true;
        }
        if (E->Kind == ExprKind::Var &&
            (Aliases.count(varKey(E->Var)) || E->Var.Kind == MedVar::Stack ||
             (E->Var.Kind == MedVar::Reg &&
              (E->Var.RegOff == TRI.StackPointer ||
               E->Var.RegOff == TRI.FramePointer))))
          Escaped = true;
        for (const auto &Operand : E->Operands)
          Pending.push_back(Operand.get());
      }
    });
  });
  if (Escaped || !Budget)
    return;
  for (const auto &[S, At, Bytes] : Candidates) {
    const auto First = ReadBytes.lower_bound(At);
    const auto End = ReadBytes.lower_bound(At + Bytes);
    if (First == End) {
      // Preserve a possible branch destination at the eliminated write.
      const va_t Address = S->Addr;
      *S = HighStmt{};
      S->Kind = StmtKind::Block;
      S->Addr = Address;
    } else if (S->StoreVal->Type->Kind == NdTypeKind::Int && Bytes <= 16) {
      const auto Kept = uint16_t(*std::prev(End) - At + 1);
      if (Kept < Bytes)
        S->StoreVal = frameValuePrefix(S->StoreVal, Kept);
    }
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

//===- PermanentConjuncts.h - Bounded permanent assertion preparation ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_BV_PERMANENTCONJUNCTS_H
#define NEVERD_SOLVER_BV_PERMANENTCONJUNCTS_H

#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::solver::detail {

using PermanentConjunct = std::pair<symbolic::SymRef, bool>;

// Large path predicates can contain thousands of small Boolean wrappers.
// Bound this optional preparation independently of bit-blasting and SAT search;
// a smaller caller-supplied allowance still takes precedence.
inline constexpr unsigned MaxPermanentConjunctWork = 1U << 18;

// Each returned literal is entailed by asserting Pred permanently. The caller
// must still assert the complete original predicate. Charge every worklist pop
// (including revisits), inspected operand edge and copied or inspected constant
// word before doing that work. Refusal discards all preparation; it is not a
// solver result.
inline std::optional<llvm::SmallVector<PermanentConjunct, 32>>
collectPermanentConjuncts(const symbolic::SymContext &Ctx,
                          symbolic::SymRef Pred,
                          unsigned MaxWork = MaxPermanentConjunctWork) {
  using symbolic::SymOp;
  using symbolic::SymRef;
  unsigned Remaining = std::min(MaxWork, MaxPermanentConjunctWork);
  auto Charge = [&](size_t Amount) {
    if (Amount > Remaining)
      return false;
    Remaining -= static_cast<unsigned>(Amount);
    return true;
  };
  auto ValidBoolean = [&](SymRef Ref) {
    return Ref.isValid() && Ref.index() < Ctx.numNodes() && Ctx.width(Ref) == 1;
  };
  if (!Remaining || !ValidBoolean(Pred))
    return std::nullopt;
  llvm::SmallVector<PermanentConjunct, 32> Work{{Pred, true}}, Leaves;
  llvm::DenseSet<uint64_t> Seen;
  while (!Work.empty()) {
    if (!Charge(1))
      return std::nullopt;
    const auto [At, Positive] = Work.pop_back_val();
    if (!ValidBoolean(At))
      return std::nullopt;
    if (!Seen.insert((uint64_t(At.index()) << 1) | Positive).second)
      continue;
    const auto Op = Ctx.op(At);
    const auto Args = Ctx.operands(At);
    if ((Op == SymOp::And && Positive) || (Op == SymOp::Or && !Positive)) {
      if (Args.empty() || !Charge(Args.size()))
        return std::nullopt;
      for (auto A : Args) {
        if (!ValidBoolean(A))
          return std::nullopt;
        Work.push_back({A, Positive});
      }
      continue;
    }
    if (Op == SymOp::Not) {
      if (Args.size() != 1 || !Charge(1) || !ValidBoolean(Args[0]))
        return std::nullopt;
      Work.push_back({Args[0], !Positive});
      continue;
    }
    if (Op == SymOp::Eq) {
      if (Args.size() != 2 || !Charge(2))
        return std::nullopt;
      for (auto A : Args)
        if (!A.isValid() || A.index() >= Ctx.numNodes())
          return std::nullopt;
      bool Expanded = false;
      for (unsigned I = 0; I != 2; ++I) {
        const auto Number = Args[I], Other = Args[1 - I];
        if (!Ctx.isConst(Number) || Ctx.op(Other) != SymOp::ZExt ||
            Ctx.numOperands(Other) != 1 || Ctx.width(Other) <= 1 ||
            Ctx.width(Other) != Ctx.width(Number))
          continue;
        if (!Charge(1))
          return std::nullopt;
        const auto Bit = Ctx.operand(Other, 0);
        if (!ValidBoolean(Bit))
          continue;
        // One full-word copy and two bounded scans of that copy.
        if (!Charge(3 * ((Ctx.width(Number) - 1) / 64 + 1)))
          return std::nullopt;
        const auto Value = Ctx.constValue(Number);
        const bool IsZero = Value.isZero(), IsOne = Value.isOne();
        if (!IsZero && !IsOne)
          continue;
        Work.push_back({Bit, Positive == IsOne});
        Expanded = true;
        break;
      }
      if (Expanded)
        continue;
    }
    Leaves.push_back({At, Positive});
  }
  std::sort(Leaves.begin(), Leaves.end(), [](auto A, auto B) {
    return A.first.index() != B.first.index()
               ? A.first.index() < B.first.index()
               : A.second < B.second;
  });
  return Leaves;
}

} // namespace neverd::solver::detail

#endif

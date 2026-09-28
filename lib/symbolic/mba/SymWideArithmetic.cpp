//===- SymWideArithmetic.cpp - Proved split-word recovery -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymWideArithmetic.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace neverd::symbolic {

SymRef recoverSplitWordArithmetic(SymContext &Ctx, SymRef Expr,
                                  SynthVerifyFn Verify) {
  if (!Expr.isValid() || Ctx.op(Expr) != SymOp::Concat ||
      Ctx.numOperands(Expr) != 2)
    return Expr;
  const SymRef Upper = Ctx.operand(Expr, 0);
  const SymRef Lower = Ctx.operand(Expr, 1);
  const uint32_t Width = Ctx.width(Lower);
  if (Width < 8 || Width != Ctx.width(Upper) || Ctx.op(Lower) != SymOp::Add)
    return Expr;

  llvm::SmallVector<SymRef, 6> Inputs;
  llvm::SmallVector<SymRef, 32> Work{Expr};
  llvm::DenseSet<uint32_t> Seen;
  while (!Work.empty()) {
    const SymRef Current = Work.pop_back_val();
    if (!Seen.insert(Current.index()).second)
      continue;
    if (Ctx.isVar(Current)) {
      if (Ctx.width(Current) != Width)
        return Expr;
      Inputs.push_back(Current);
      if (Inputs.size() > 6)
        return Expr;
      continue;
    }
    const auto Operands = Ctx.operands(Current);
    Work.append(Operands.begin(), Operands.end());
  }
  if (Inputs.size() != 4 && Inputs.size() != 6)
    return Expr;
  std::sort(Inputs.begin(), Inputs.end(),
            [](SymRef A, SymRef B) { return A.index() < B.index(); });

  // A low word cannot depend on a high input of an ordinary packed
  // arithmetic expression. Partition by actual dependence before trying
  // pairings; this also keeps the six-input search finite.
  llvm::DenseSet<uint32_t> LowInputIds;
  Work = {Lower};
  Seen.clear();
  while (!Work.empty()) {
    const SymRef Current = Work.pop_back_val();
    if (!Seen.insert(Current.index()).second)
      continue;
    if (Ctx.isVar(Current)) {
      LowInputIds.insert(Current.index());
      continue;
    }
    const auto Operands = Ctx.operands(Current);
    Work.append(Operands.begin(), Operands.end());
  }
  llvm::SmallVector<SymRef, 3> LowInputs, HighInputs;
  for (SymRef Input : Inputs) {
    if (LowInputIds.contains(Input.index()))
      LowInputs.push_back(Input);
    else
      HighInputs.push_back(Input);
  }
  if (LowInputs.size() != HighInputs.size())
    return Expr;
  const unsigned NumWords = LowInputs.size();

  // A handful of deterministic assignments reject wrong pairings cheaply.
  // They never authorize a rewrite; only Verify can do that.
  std::vector<llvm::APInt> Values;
  Values.reserve(Ctx.numVars());
  for (uint32_t Id = 0; Id < Ctx.numVars(); ++Id)
    Values.emplace_back(Ctx.varInfo(Id).Width, 0);
  // At zero inputs, packed addition and subtraction vanish. Any remaining
  // value is the modular offset required by an affine candidate.
  const SymRef Offset = Ctx.mkConst(Ctx.eval(Expr, Values));
  llvm::SmallVector<llvm::APInt, 8> Expected;
  const uint64_t Seeds[8][6] = {{0, 0, 0, 0, 0, 0},
                                {1, 2, 4, 8, 16, 32},
                                {~0ULL, 1, 0, 0, 0, 0},
                                {0, ~0ULL, 1, 0, 0, 0},
                                {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL},
                                {0x13579bdfULL, 0x2468ace0ULL, 0x31415926ULL,
                                 0x27182818ULL, 0xdeadbeefULL, 0x10293847ULL},
                                {0x80000000ULL, 0x7fffffffULL, 0xffffffffULL, 1,
                                 0x80000001ULL, 0x7ffffffeULL},
                                {0xaaaaaaaaULL, 0x55555555ULL, 0x33333333ULL,
                                 0xccccccccULL, 0xf0f0f0f0ULL, 0x0f0f0f0fULL}};
  for (unsigned Sample = 0; Sample < 8; ++Sample) {
    for (unsigned I = 0; I < Inputs.size(); ++I)
      Values[Ctx.varId(Inputs[I])] = llvm::APInt(Width, Seeds[Sample][I]);
    Expected.push_back(Ctx.eval(Expr, Values));
  }

  std::array<unsigned, 3> HighOrder{0, 1, 2};
  llvm::DenseSet<uint32_t> Tried;
  do {
    std::array<SymRef, 3> Packed;
    for (unsigned I = 0; I < NumWords; ++I)
      Packed[I] = Ctx.mkConcat(HighInputs[HighOrder[I]], LowInputs[I]);
    std::array<unsigned, 3> Order{0, 1, 2};
    do {
      for (unsigned Signs = 0; Signs < (1u << (NumWords - 1)); ++Signs) {
        SymRef Arithmetic = Packed[Order[0]];
        for (unsigned I = 1; I < NumWords; ++I)
          Arithmetic = Signs & (1u << (I - 1))
                           ? Ctx.mkSub(Arithmetic, Packed[Order[I]])
                           : Ctx.mkAdd(Arithmetic, Packed[Order[I]]);
        const SymRef Candidate = Ctx.isConstZero(Offset)
                                     ? Arithmetic
                                     : Ctx.mkAdd(Arithmetic, Offset);
        if (!Tried.insert(Candidate.index()).second ||
            Ctx.readabilityCost(Candidate) >= Ctx.readabilityCost(Expr))
          continue;
        bool Matches = true;
        for (unsigned Sample = 0; Sample < 8 && Matches; ++Sample) {
          for (unsigned I = 0; I < Inputs.size(); ++I)
            Values[Ctx.varId(Inputs[I])] = llvm::APInt(Width, Seeds[Sample][I]);
          Matches = Ctx.eval(Candidate, Values) == Expected[Sample];
        }
        if (Matches &&
            Verify(Ctx, Expr, Candidate) == SynthVerification::Equivalent)
          return Candidate;
      }
    } while (std::next_permutation(Order.begin(), Order.begin() + NumWords));
  } while (
      std::next_permutation(HighOrder.begin(), HighOrder.begin() + NumWords));
  return Expr;
}

} // namespace neverd::symbolic

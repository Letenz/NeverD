//===- CompleteModel.cpp - Complete concrete witness validation ===========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CompleteModel.h"

#include "ProofNode.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

#include <map>

namespace neverd::analysis::complete_model {
Result verify(const symbolic::SymContext &C, symbolic::SymRef Predicate,
              const solver::BitVectorModel &M, uint64_t MaxWords,
              uint32_t MaxWidth) {
  using namespace symbolic;
  Result R;
  const auto Valid = [&](SymRef X) { return X && X.index() < C.numNodes(); };
  if (!Valid(Predicate) || C.width(Predicate) != 1)
    return R;
  if (!MaxWords) {
    R.Answer = Verdict::BudgetExceeded;
    return R;
  }
  ++R.Words;
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 64> Pending{Predicate};
  std::map<uint32_t, llvm::APInt> Values;
  while (!Pending.empty()) {
    const auto X = Pending.pop_back_val();
    if (!Valid(X))
      return R;
    if (!Seen.insert(X.index()).second)
      continue;
    const auto &N = C.node(X);
    const auto A = C.operands(X);
    if (!N.Width || (MaxWidth && N.Width > MaxWidth))
      return R;
    const uint64_t Cost =
        1 + uint64_t(A.size()) + (uint64_t(N.Width) + 63) / 64;
    if (Cost > MaxWords - R.Words) {
      R.Answer = Verdict::BudgetExceeded;
      return R;
    }
    R.Words += Cost;
    ++R.Nodes;
    R.Edges += A.size();
    for (auto V : A)
      if (!Valid(V) || V.index() >= X.index())
        return R;
    if (!detail::validProofNode(C, X))
      return R;
    if (N.Op == SymOp::Var) {
      const auto V = M.value(C.varId(X));
      if (!V) {
        R.Answer = Verdict::MissingVariable;
        return R;
      }
      if (V->getBitWidth() != N.Width)
        return R;
      Values.emplace(C.varId(X), *V);
      ++R.Variables;
    }
    Pending.append(A.begin(), A.end());
  }
  // The context is immutable during this call. Use its authoritative APInt
  // evaluator after checking every node and exact-width variable. The lookup
  // still fails closed if a required variable was missed by the inspection.
  bool Missing = false;
  SymEvalPlan Plan(C, Predicate);
  const auto Value = Plan.evalWith([&](uint32_t Id) -> const llvm::APInt * {
    const auto It = Values.find(Id);
    if (It == Values.end()) {
      Missing = true;
      return nullptr;
    }
    return &It->second;
  });
  R.Answer = Missing         ? Verdict::MissingVariable
             : Value.isOne() ? Verdict::Satisfied
                             : Verdict::Refuted;
  return R;
}
} // namespace neverd::analysis::complete_model

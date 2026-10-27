//===- SymExprEval.cpp - Evaluating symbolic expressions -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements one-shot expression evaluation, the reusable \c SymEvalPlan,
/// and the width-exact operator semantics shared with MBA verification.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

namespace neverd::symbolic {

bool SymContext::fitsU64(SymRef R) const {
  llvm::DenseSet<uint32_t> Seen;
  llvm::SmallVector<SymRef, 32> Work{R};
  while (!Work.empty()) {
    SymRef Cur = Work.pop_back_val();
    if (!Seen.insert(Cur.index()).second)
      continue;
    if (width(Cur) > 64)
      return false;
    for (SymRef C : operands(Cur))
      Work.push_back(C);
  }
  return true;
}

llvm::APInt SymContext::eval(SymRef R,
                             llvm::ArrayRef<llvm::APInt> VarVals) const {
  SymEvalPlan Plan(*this, R);
  return Plan.eval(VarVals);
}

uint64_t SymContext::evalU64(SymRef R, llvm::ArrayRef<uint64_t> VarVals) const {
  SymEvalPlan Plan(*this, R);
  return Plan.evalU64(VarVals);
}

//===----------------------------------------------------------------------===//
// SymEvalPlan
//===----------------------------------------------------------------------===//

SymEvalPlan::SymEvalPlan(const SymContext &Ctx, SymRef Root)
    : Ctx(Ctx), Root(Root) {
  // Iterative post-order so a deeply nested expression cannot overflow the
  // stack; obfuscators emit exactly that shape on purpose.
  // Zero marks a scheduled node; completed entries contain slot + 1. One
  // table therefore tracks visitation and resolves operand slots without
  // allocating a second set for the same reachable nodes.
  llvm::SmallDenseMap<uint32_t, uint32_t,
                      limits::symbolic::kSymExprTraversalInlineBuckets>
      Slot;
  llvm::SmallVector<std::pair<SymRef, bool>, 64> Work{{Root, false}};
  llvm::SmallVector<SymRef, 64> PostOrder;
  size_t OperandCount = 0;

  while (!Work.empty()) {
    auto [Cur, Expanded] = Work.pop_back_val();
    if (Expanded) {
      auto Position = Slot.find(Cur.index());
      if (Position->second)
        continue;
      Position->second = static_cast<uint32_t>(PostOrder.size()) + 1;
      PostOrder.push_back(Cur);
      OperandCount += Ctx.numOperands(Cur);
      continue;
    }
    if (!Slot.try_emplace(Cur.index(), 0).second)
      continue;
    Work.emplace_back(Cur, true);
    for (SymRef C : Ctx.operands(Cur))
      Work.emplace_back(C, false);
  }

  // Lower to flat steps.  Operand slots are resolved here, once, so the
  // evaluation loops never touch the hash map or the DAG.
  Steps.reserve(PostOrder.size());
  ArgSlots.reserve(OperandCount);
  for (SymRef R : PostOrder) {
    const SymNode &N = Ctx.node(R);
    if (N.Width > 64)
      FitsU64 = false;
    if (N.Op == SymOp::Var)
      Vars.push_back(static_cast<uint32_t>(N.Aux));

    Step S;
    S.Op = N.Op;
    S.Width = N.Width;
    S.Aux = N.Aux;
    S.Node = R;
    S.FirstArg = static_cast<uint32_t>(ArgSlots.size());
    S.NumArgs = N.NumOperands;
    for (SymRef C : Ctx.operands(R))
      ArgSlots.push_back(Slot.find(C.index())->second - 1);
    Steps.push_back(S);
  }

  RootSlot = Slot.find(Root.index())->second - 1;

  // The lowering already visits every reachable variable. Retain the public
  // sorted, unique order without a second DAG walk and its temporary set.
  std::sort(Vars.begin(), Vars.end());
  Vars.erase(std::unique(Vars.begin(), Vars.end()), Vars.end());

  if (FitsU64)
    ScratchU64.resize(Steps.size());
}

uint64_t SymEvalPlan::evalU64(llvm::ArrayRef<uint64_t> VarVals) {
  return evalU64With(
      [&](uint32_t Id) { return Id < VarVals.size() ? VarVals[Id] : 0; });
}

namespace {

// Keep arbitrary-precision construction and cleanup out of the common native
// evaluator's stack frame. General operators still have one semantics owner.
LLVM_ATTRIBUTE_NOINLINE
uint64_t evalU64ViaAP(const SymContext &Ctx, SymRef Node,
                      llvm::ArrayRef<uint32_t> Args, const uint64_t *Values) {
  llvm::SmallVector<llvm::APInt, 4> APArgs;
  for (size_t K = 0; K < Args.size(); ++K)
    APArgs.emplace_back(Ctx.width(Ctx.operand(Node, K)), Values[Args[K]]);
  return evalNodeAP(Ctx, Node, APArgs).getZExtValue();
}

} // namespace

uint64_t
SymEvalPlan::evalU64With(llvm::function_ref<uint64_t(uint32_t)> Lookup) {
  assert(FitsU64 && "evalU64 requires every node to fit in 64 bits");

  auto mask = [](uint64_t V, uint32_t W) -> uint64_t {
    return W >= 64 ? V : (V & ((uint64_t(1) << W) - 1));
  };

  const uint32_t *Args = ArgSlots.data();
  uint64_t *S = ScratchU64.data();

  for (size_t I = 0; I < Steps.size(); ++I) {
    const Step &St = Steps[I];
    const uint32_t *A = Args + St.FirstArg;
    uint32_t W = St.Width;
    uint64_t Res = 0;

    switch (St.Op) {
    case SymOp::Const:
      // The constant builder already truncated this native-width literal.
      S[I] = St.Aux;
      continue;
    case SymOp::Var:
      Res = Lookup(static_cast<uint32_t>(St.Aux));
      break;
    case SymOp::Add:
      // Canonical associative nodes have at least two operands. Evaluate
      // that common pair directly, then fold any remaining operands.
      Res = S[A[0]] + S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res += S[A[K]];
      break;
    case SymOp::Mul:
      Res = S[A[0]] * S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res *= S[A[K]];
      break;
    case SymOp::And:
      // Every earlier slot is normalized to its declared width. These
      // bitwise operators preserve that invariant without another mask.
      Res = S[A[0]] & S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res &= S[A[K]];
      S[I] = Res;
      continue;
    case SymOp::Or:
      Res = S[A[0]] | S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res |= S[A[K]];
      S[I] = Res;
      continue;
    case SymOp::Xor:
      Res = S[A[0]] ^ S[A[1]];
      for (uint32_t K = 2; K < St.NumArgs; ++K)
        Res ^= S[A[K]];
      S[I] = Res;
      continue;
    case SymOp::Not:
      Res = ~S[A[0]];
      break;
    case SymOp::Shl: {
      uint64_t Sh = S[A[1]];
      Res = Sh >= W ? 0 : (S[A[0]] << Sh);
      break;
    }
    case SymOp::LShr: {
      uint64_t Sh = S[A[1]];
      S[I] = Sh >= W ? 0 : (S[A[0]] >> Sh);
      continue;
    }
    case SymOp::AShr: {
      uint64_t V = S[A[0]], Sh = S[A[1]];
      bool Neg = (V >> (W - 1)) & 1;
      if (Sh == 0)
        Res = V;
      else if (Sh >= W)
        Res = Neg ? ~uint64_t(0) : 0;
      else
        Res = Neg ? ((V >> Sh) | ~((uint64_t(1) << (W - Sh)) - 1)) : (V >> Sh);
      break;
    }
    case SymOp::UDiv: {
      uint64_t X = S[A[0]], Y = S[A[1]];
      Res = Y == 0 ? ~uint64_t(0) : (X / Y);
      break;
    }
    case SymOp::URem: {
      uint64_t X = S[A[0]], Y = S[A[1]];
      S[I] = Y == 0 ? X : (X % Y);
      continue;
    }
    case SymOp::Eq:
      S[I] = S[A[0]] == S[A[1]];
      continue;
    case SymOp::Ult:
      S[I] = S[A[0]] < S[A[1]];
      continue;
    case SymOp::Ule:
      S[I] = S[A[0]] <= S[A[1]];
      continue;
    case SymOp::Ite:
      S[I] = S[A[0]] ? S[A[1]] : S[A[2]];
      continue;
    case SymOp::ZExt:
      S[I] = S[A[0]];
      continue;
    case SymOp::Extract:
      Res = S[A[0]] >> St.Aux;
      break;
    default: {
      // The remaining operators need width-exact or signed semantics that are
      // stated once in evalNodeAP.  They are rare in the MBA inner loop, so
      // reconstructing operand APInts here costs little and avoids a second,
      // divergent copy of the semantics.
      Res = evalU64ViaAP(Ctx, St.Node, {A, St.NumArgs}, S);
      break;
    }
    }

    S[I] = mask(Res, W);
  }

  return Steps.empty() ? 0 : ScratchU64[RootSlot];
}

llvm::APInt SymEvalPlan::eval(llvm::ArrayRef<llvm::APInt> VarVals) {
  return evalWith([&](uint32_t Id) -> const llvm::APInt * {
    return Id < VarVals.size() ? &VarVals[Id] : nullptr;
  });
}

llvm::APInt SymEvalPlan::evalWith(
    llvm::function_ref<const llvm::APInt *(uint32_t)> Lookup) {
  // Native-width measurements never use APInt scratch. Allocate it only
  // when this entry point is called, retaining it for subsequent evaluations.
  if (ScratchAP.empty())
    ScratchAP.resize(Steps.size());
  for (size_t I = 0; I < Steps.size(); ++I) {
    const Step &St = Steps[I];
    const uint32_t *A = ArgSlots.data() + St.FirstArg;

    if (St.Op == SymOp::Const) {
      ScratchAP[I] = Ctx.constValue(St.Node);
      continue;
    }
    if (St.Op == SymOp::Var) {
      const llvm::APInt *V = Lookup(static_cast<uint32_t>(St.Aux));
      ScratchAP[I] =
          V ? (V->getBitWidth() == St.Width ? *V : V->zextOrTrunc(St.Width))
            : llvm::APInt(St.Width, 0);
      continue;
    }

    llvm::SmallVector<llvm::APInt, 4> APArgs;
    APArgs.reserve(St.NumArgs);
    for (uint32_t K = 0; K < St.NumArgs; ++K)
      APArgs.push_back(ScratchAP[A[K]]);
    ScratchAP[I] = evalNodeAP(Ctx, St.Node, APArgs);
  }

  return Steps.empty() ? llvm::APInt(Ctx.width(Root), 0) : ScratchAP[RootSlot];
}

llvm::APInt evalNodeAP(const SymContext &Ctx, SymRef R,
                       llvm::ArrayRef<llvm::APInt> Args) {
  const SymNode &N = Ctx.node(R);
  uint32_t W = N.Width;

  switch (N.Op) {
  case SymOp::Const:
    return Ctx.constValue(R);
  case SymOp::Var:
    return llvm::APInt(W, 0);
  case SymOp::Add: {
    llvm::APInt Acc(W, 0);
    for (const auto &A : Args)
      Acc += A;
    return Acc;
  }
  case SymOp::Mul: {
    llvm::APInt Acc(W, 1);
    for (const auto &A : Args)
      Acc *= A;
    return Acc;
  }
  case SymOp::And: {
    llvm::APInt Acc = llvm::APInt::getAllOnes(W);
    for (const auto &A : Args)
      Acc &= A;
    return Acc;
  }
  case SymOp::Or: {
    llvm::APInt Acc(W, 0);
    for (const auto &A : Args)
      Acc |= A;
    return Acc;
  }
  case SymOp::Xor: {
    llvm::APInt Acc(W, 0);
    for (const auto &A : Args)
      Acc ^= A;
    return Acc;
  }
  case SymOp::Not:
    return ~Args[0];
  case SymOp::Shl:
    return Args[1].uge(W) ? llvm::APInt(W, 0)
                          : Args[0].shl(Args[1].getZExtValue());
  case SymOp::LShr:
    return Args[1].uge(W) ? llvm::APInt(W, 0)
                          : Args[0].lshr(Args[1].getZExtValue());
  case SymOp::AShr:
    if (Args[1].uge(W))
      return Args[0].isNegative() ? llvm::APInt::getAllOnes(W)
                                  : llvm::APInt(W, 0);
    return Args[0].ashr(Args[1].getZExtValue());
  case SymOp::UDiv:
    return Args[1].isZero() ? llvm::APInt::getAllOnes(W)
                            : Args[0].udiv(Args[1]);
  case SymOp::SDiv:
    if (Args[1].isZero())
      return Args[0].isNonNegative() ? llvm::APInt::getAllOnes(W)
                                     : llvm::APInt(W, 1);
    if (Args[0].isMinSignedValue() && Args[1].isAllOnes())
      return Args[0];
    return Args[0].sdiv(Args[1]);
  case SymOp::URem:
    return Args[1].isZero() ? Args[0] : Args[0].urem(Args[1]);
  case SymOp::SRem:
    if (Args[1].isZero())
      return Args[0];
    if (Args[0].isMinSignedValue() && Args[1].isAllOnes())
      return llvm::APInt(W, 0);
    return Args[0].srem(Args[1]);
  case SymOp::Rol:
    return Args[0].rotl(Args[1].urem(W));
  case SymOp::Ror:
    return Args[0].rotr(Args[1].urem(W));
  case SymOp::Extract:
    return Args[0].extractBits(W, static_cast<uint32_t>(N.Aux));
  case SymOp::Concat: {
    llvm::APInt Acc(W, 0);
    uint32_t Shift = W;
    for (const auto &A : Args) {
      Shift -= A.getBitWidth();
      Acc |= A.zext(W).shl(Shift);
    }
    return Acc;
  }
  case SymOp::ZExt:
    return Args[0].zext(W);
  case SymOp::SExt:
    return Args[0].sext(W);
  case SymOp::Ite:
    return Args[0].isZero() ? Args[2] : Args[1];
  case SymOp::Eq:
    return llvm::APInt(1, Args[0] == Args[1]);
  case SymOp::Ult:
    return llvm::APInt(1, Args[0].ult(Args[1]));
  case SymOp::Ule:
    return llvm::APInt(1, Args[0].ule(Args[1]));
  case SymOp::Slt:
    return llvm::APInt(1, Args[0].slt(Args[1]));
  case SymOp::Sle:
    return llvm::APInt(1, Args[0].sle(Args[1]));
  }
  llvm_unreachable("unhandled SymOp in evalNodeAP");
}

} // namespace neverd::symbolic

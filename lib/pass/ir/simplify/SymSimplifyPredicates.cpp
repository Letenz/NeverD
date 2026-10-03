//===- SymSimplifyPredicates.cpp - Exact modular predicate domains
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymSimplifyDetail.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/ValueHandle.h"

#include <optional>

namespace neverd {
namespace {

// A circular interval is an exact truth set, never an overapproximation. All
// nonconstant operands must retain the same SSA identity. This is integer
// algebra over the complete bitvector domain, not path reasoning or sampling.
struct PredicateDomain {
  llvm::Value *Anchor;
  llvm::ConstantRange True;
};

struct AffineValue {
  llvm::Value *Anchor;
  llvm::APInt Offset;
  bool Negative = false;
};

class WorkBudget {
  size_t Remaining;

public:
  explicit WorkBudget(size_t Limit) : Remaining(Limit) {}
  bool spend(size_t Amount = 1) {
    if (Amount > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Amount;
    return true;
  }
  bool empty() const { return Remaining == 0; }
};

bool supportedType(const llvm::Value *V) {
  return V->getType()->isIntegerTy() &&
         V->getType()->getIntegerBitWidth() <= 512;
}

bool removable(const llvm::Instruction &I) {
  if (!supportedType(&I))
    return false;
  switch (I.getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::ICmp:
    return true;
  default:
    return false;
  }
}

class PredicateAnalysis {
  WorkBudget &Budget;
  llvm::DenseMap<llvm::Value *, std::optional<AffineValue>> AffineMemo;
  llvm::DenseMap<llvm::Value *, std::optional<PredicateDomain>> SignMemo;
  llvm::DenseMap<llvm::Value *, std::optional<PredicateDomain>> PredicateMemo;

  bool enter(llvm::Value *V, unsigned Depth) {
    // The depth limit bounds the C++ stack even for callers with an unlimited
    // work policy. Width-weighted work also bounds APInt and interval algebra.
    return Depth <= 128 && supportedType(V) &&
           Budget.spend(1 + V->getType()->getIntegerBitWidth() / 64);
  }

  std::optional<AffineValue> affine(llvm::Value *V, unsigned Depth) {
    if (!enter(V, Depth))
      return std::nullopt;
    if (auto It = AffineMemo.find(V); It != AffineMemo.end())
      return It->second;
    auto Result = deriveAffine(V, Depth);
    AffineMemo[V] = Result;
    return Result;
  }

  std::optional<AffineValue> deriveAffine(llvm::Value *V, unsigned Depth) {
    using namespace llvm;
    AffineValue Original{V, APInt(V->getType()->getIntegerBitWidth(), 0)};
    const auto *I = dyn_cast<BinaryOperator>(V);
    // An annotated/unsupported operation may remain as the original anchor;
    // never reason through its poison-producing semantics.
    if (!I || I->hasPoisonGeneratingAnnotations())
      return Original;
    const auto *Left = dyn_cast<ConstantInt>(I->getOperand(0));
    const auto *Right = dyn_cast<ConstantInt>(I->getOperand(1));
    if (!Left && !Right)
      return Original;
    const auto *C = Right ? Right : Left;
    if (I->getOpcode() != Instruction::Add &&
        I->getOpcode() != Instruction::Sub &&
        !(I->getOpcode() == Instruction::Xor && C->isMinusOne()))
      return Original;
    auto A = affine(I->getOperand(Right ? 0 : 1), Depth + 1);
    if (!A)
      return std::nullopt;
    if (I->getOpcode() == Instruction::Add)
      A->Offset += C->getValue();
    else if (I->getOpcode() == Instruction::Xor) {
      A->Negative = !A->Negative;
      A->Offset = ~A->Offset;
    } else if (Right)
      A->Offset -= C->getValue();
    else {
      A->Negative = !A->Negative;
      A->Offset = C->getValue() - A->Offset;
    }
    return A;
  }

  PredicateDomain preimage(const AffineValue &A, llvm::ConstantRange R) {
    R = R.subtract(A.Offset);
    // Negating the half-open interval [L,U) gives [1-U,1-L), modulo 2^W.
    if (A.Negative && !R.isEmptySet() && !R.isFullSet())
      R = llvm::ConstantRange(llvm::APInt(R.getBitWidth(), 1) - R.getUpper(),
                              llvm::APInt(R.getBitWidth(), 1) - R.getLower());
    return {A.Anchor, R};
  }

  std::optional<PredicateDomain> combine(PredicateDomain A, PredicateDomain B,
                                         unsigned Opcode) {
    if (!Budget.spend(1 + (A.True.getBitWidth() + B.True.getBitWidth()) / 64))
      return std::nullopt;
    // Only literal constants have no anchor. An anchor whose truth set became
    // full/empty still carries its original poison dependence.
    if (!A.Anchor && B.Anchor) {
      A.True = llvm::ConstantRange(B.True.getBitWidth(), A.True.isFullSet());
      A.Anchor = B.Anchor;
    }
    if (!B.Anchor && A.Anchor) {
      B.True = llvm::ConstantRange(A.True.getBitWidth(), B.True.isFullSet());
      B.Anchor = A.Anchor;
    }
    if (A.Anchor != B.Anchor || A.True.getBitWidth() != B.True.getBitWidth())
      return std::nullopt;
    auto R = Opcode == llvm::Instruction::And
                 ? A.True.exactIntersectWith(B.True)
                 : A.True.exactUnionWith(B.True);
    // A disconnected set cannot be represented by one interval. In
    // particular, do not use ConstantRange's widening union/intersection.
    return R ? std::optional<PredicateDomain>({A.Anchor, *R}) : std::nullopt;
  }

  std::optional<PredicateDomain> sign(llvm::Value *V, unsigned Depth) {
    if (!enter(V, Depth))
      return std::nullopt;
    if (auto It = SignMemo.find(V); It != SignMemo.end())
      return It->second;
    auto Result = deriveSign(V, Depth);
    SignMemo[V] = Result;
    return Result;
  }

  std::optional<PredicateDomain> deriveSign(llvm::Value *V, unsigned Depth) {
    using namespace llvm;
    const unsigned Width = V->getType()->getIntegerBitWidth();
    if (const auto *C = dyn_cast<ConstantInt>(V))
      return PredicateDomain{nullptr,
                             ConstantRange(Width, C->getValue().isNegative())};
    if (const auto *I = dyn_cast<BinaryOperator>(V);
        I && !I->hasPoisonGeneratingAnnotations()) {
      if (I->getOpcode() == Instruction::And ||
          I->getOpcode() == Instruction::Or) {
        auto A = sign(I->getOperand(0), Depth + 1);
        auto B = sign(I->getOperand(1), Depth + 1);
        if (A && B)
          if (auto R = combine(*A, *B, I->getOpcode()))
            return R;
      } else if (I->getOpcode() == Instruction::Xor) {
        const auto *C = dyn_cast<ConstantInt>(I->getOperand(1));
        unsigned Index = 0;
        if (!C) {
          C = dyn_cast<ConstantInt>(I->getOperand(0));
          Index = 1;
        }
        if (C) {
          auto A = sign(I->getOperand(Index), Depth + 1);
          if (A && C->getValue().isNegative())
            A->True = A->True.inverse();
          return A;
        }
      }
    }
    auto A = affine(V, Depth + 1);
    if (!A)
      return std::nullopt;
    return preimage(
        *A, ConstantRange(APInt::getSignedMinValue(Width), APInt(Width, 0)));
  }

  std::optional<PredicateDomain> derivePredicate(llvm::Value *V,
                                                 unsigned Depth) {
    using namespace llvm;
    if (const auto *C = dyn_cast<ConstantInt>(V))
      return PredicateDomain{nullptr, ConstantRange(1, !C->isZero())};
    const auto *I = dyn_cast<Instruction>(V);
    if (!I || I->hasPoisonGeneratingAnnotations())
      return std::nullopt;
    if (I->getOpcode() == Instruction::And ||
        I->getOpcode() == Instruction::Or) {
      auto A = predicate(I->getOperand(0), Depth + 1);
      auto B = predicate(I->getOperand(1), Depth + 1);
      return A && B ? combine(*A, *B, I->getOpcode()) : std::nullopt;
    }
    if (I->getOpcode() == Instruction::Xor) {
      const auto *C = dyn_cast<ConstantInt>(I->getOperand(1));
      unsigned Index = 0;
      if (!C) {
        C = dyn_cast<ConstantInt>(I->getOperand(0));
        Index = 1;
      }
      auto A = C ? predicate(I->getOperand(Index), Depth + 1) : std::nullopt;
      if (A && C->isOne())
        A->True = A->True.inverse();
      return A;
    }
    const auto *Cmp = dyn_cast<ICmpInst>(I);
    if (!Cmp)
      return std::nullopt;
    Value *L = Cmp->getOperand(0);
    const auto *C = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    auto Pred = Cmp->getPredicate();
    if (!C) {
      C = dyn_cast<ConstantInt>(L);
      L = Cmp->getOperand(1);
      Pred = ICmpInst::getSwappedPredicate(Pred);
    }
    if (!C || !supportedType(L) || !Budget.spend(1 + C->getBitWidth() / 64))
      return std::nullopt;
    auto R = ConstantRange::makeExactICmpRegion(Pred, C->getValue());
    ConstantRange Negative(APInt::getSignedMinValue(R.getBitWidth()),
                           APInt(R.getBitWidth(), 0));
    if (R == Negative || R == Negative.inverse()) {
      auto A = sign(L, Depth + 1);
      if (A && R != Negative)
        A->True = A->True.inverse();
      return A;
    }
    auto A = affine(L, Depth + 1);
    return A ? std::optional<PredicateDomain>(preimage(*A, R)) : std::nullopt;
  }

public:
  explicit PredicateAnalysis(WorkBudget &Budget) : Budget(Budget) {}

  void clear() {
    AffineMemo.clear();
    SignMemo.clear();
    PredicateMemo.clear();
  }

  std::optional<PredicateDomain> predicate(llvm::Value *V, unsigned Depth = 0) {
    if (!V->getType()->isIntegerTy(1) || !enter(V, Depth))
      return std::nullopt;
    if (auto It = PredicateMemo.find(V); It != PredicateMemo.end())
      return It->second;
    auto Result = derivePredicate(V, Depth);
    PredicateMemo[V] = Result;
    return Result;
  }

  bool hasStableInputs(llvm::Value *Root) {
    llvm::SmallVector<llvm::Value *, 32> Work{Root};
    llvm::DenseSet<llvm::Value *> Seen;
    while (!Work.empty()) {
      if (!Budget.spend())
        return false;
      auto *V = Work.pop_back_val();
      if (!Seen.insert(V).second)
        continue;
      if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V))
        return false;
      const auto *U = llvm::dyn_cast<llvm::User>(V);
      if (!U || llvm::isa<llvm::GlobalValue, llvm::LoadInst, llvm::CallBase,
                          llvm::FreezeInst>(V))
        continue;
      // Check opaque arithmetic and every PHI edge as well. Repeated undef
      // uses are not a stable value. A freeze/load/call retains its own SSA
      // identity and is not correlated with other such observations.
      for (llvm::Value *Op : U->operands()) {
        if (!Budget.spend())
          return false;
        Work.push_back(Op);
      }
    }
    return true;
  }
};

// Return only instructions actually made dead, in deletion order. Shared uses
// keep their producers alive; the replacement keeps Anchor alive. Counting
// uses explicitly makes high fan-out part of the cumulative work budget.
std::optional<llvm::SmallVector<llvm::Instruction *, 32>>
findDead(llvm::Instruction *Root, llvm::Value *Anchor, WorkBudget &Budget) {
  llvm::DenseMap<llvm::Value *, size_t> Uses;
  llvm::SmallVector<llvm::Value *, 32> Work{Root};
  while (!Work.empty()) {
    if (!Budget.spend())
      return std::nullopt;
    auto *V = Work.pop_back_val();
    if (V == Anchor || Uses.contains(V))
      continue;
    const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
    if (!I || !removable(*I))
      continue;
    size_t Count = 0;
    for ([[maybe_unused]] const llvm::Use &U : V->uses()) {
      if (!Budget.spend())
        return std::nullopt;
      ++Count;
    }
    Uses[V] = Count;
    for (llvm::Value *Op : I->operands()) {
      if (!Budget.spend())
        return std::nullopt;
      Work.push_back(Op);
    }
  }
  llvm::SmallVector<llvm::Instruction *, 32> Dead;
  Work.push_back(Root);
  while (!Work.empty()) {
    if (!Budget.spend())
      return std::nullopt;
    auto *I = llvm::dyn_cast<llvm::Instruction>(Work.pop_back_val());
    if (!I || !Uses.contains(I))
      continue;
    Dead.push_back(I);
    for (llvm::Value *Op : I->operands()) {
      if (!Budget.spend())
        return std::nullopt;
      auto It = Uses.find(Op);
      if (It != Uses.end() && It->second && --It->second == 0)
        Work.push_back(Op);
    }
  }
  return Dead;
}

} // namespace

unsigned simplifyModularPredicates(llvm::Function &F,
                                   const SymSimplifyOptions &Opts) {
  WorkBudget Budget(Opts.MaxPredicateWork);
  llvm::SmallVector<llvm::WeakTrackingVH, 64> Roots;
  for (llvm::Instruction &I : llvm::instructions(F)) {
    if (!Budget.spend())
      return 0;
    if (I.getType()->isIntegerTy(1) && !I.use_empty())
      Roots.push_back(&I);
  }
  PredicateAnalysis Analysis(Budget);
  unsigned Rewrites = 0;
  for (llvm::WeakTrackingVH &Handle : llvm::reverse(Roots)) {
    if (Budget.empty())
      break;
    auto *Root = llvm::dyn_cast_or_null<llvm::Instruction>(Handle);
    if (!Root || Root->use_empty())
      continue;
    const auto P = Analysis.predicate(Root);
    if (!P || !P->Anchor || P->Anchor == Root || P->True.isEmptySet() ||
        P->True.isFullSet() || !Analysis.hasStableInputs(Root))
      continue;
    llvm::ICmpInst::Predicate Pred;
    llvm::APInt Bound(1, 0), Offset(1, 0);
    if (!Budget.spend(1 + P->True.getBitWidth() / 64))
      break;
    P->True.getEquivalentICmp(Pred, Bound, Offset);
    const size_t Cost = 1 + !Offset.isZero();
    const auto Dead = findDead(Root, P->Anchor, Budget);
    if (!Dead || Dead->size() <= Cost ||
        Dead->size() - Cost < Opts.MinInstructionsSaved ||
        !Budget.spend(Cost + Dead->size()))
      continue;
    llvm::IRBuilder<> B(Root);
    B.SetCurrentDebugLocation(Root->getDebugLoc());
    llvm::Value *V = P->Anchor;
    if (!Offset.isZero())
      V = B.CreateAdd(V, llvm::ConstantInt::get(V->getType(), Offset));
    auto *After =
        B.CreateICmp(Pred, V, llvm::ConstantInt::get(V->getType(), Bound));
    Root->replaceAllUsesWith(After);
    Analysis.clear();
    for (auto *I : *Dead) {
      assert(I->use_empty() && "predicate profitability and deletion disagree");
      I->eraseFromParent();
    }
    ++Rewrites;
  }
  return Rewrites;
}

} // namespace neverd

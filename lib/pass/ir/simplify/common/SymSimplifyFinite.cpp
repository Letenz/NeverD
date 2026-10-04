//===- SymSimplifyFinite.cpp - Exact two-valued integer slices
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymSimplifyDetail.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Transforms/Utils/Local.h"

#include <optional>

namespace neverd {
namespace {

// These are two complete possible values, not two samples. An anchor's own
// computation remains in the IR. All derived operands must refer to that same
// SSA value; similar masks, register names, or different loads do not
// correlate.
struct TwoValues {
  llvm::Value *Anchor;
  llvm::APInt AnchorFirst;
  llvm::APInt First;
  llvm::APInt Second;
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
  bool exhausted() const { return Remaining == 0; }
};

bool supportedType(llvm::Type *T) {
  return T->isIntegerTy() && T->getIntegerBitWidth() <= 512;
}

bool supportedInstruction(const llvm::Instruction &I) {
  if (!supportedType(I.getType()))
    return false;
  switch (I.getOpcode()) {
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub:
  case llvm::Instruction::Mul:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::ICmp:
  case llvm::Instruction::Select:
    return llvm::all_of(I.operands(), [](const llvm::Use &U) {
      return supportedType(U->getType());
    });
  default:
    return false;
  }
}

std::optional<TwoValues> anchorValues(llvm::Value *V) {
  const unsigned Width = V->getType()->getIntegerBitWidth();
  auto Pair = [&](llvm::APInt A, llvm::APInt B) -> std::optional<TwoValues> {
    return TwoValues{V, A, A, B};
  };
  if (Width == 1)
    return Pair(llvm::APInt(1, 0), llvm::APInt(1, 1));
  const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(V);
  if (!Op)
    return std::nullopt;
  auto *Number = llvm::dyn_cast<llvm::ConstantInt>(Op->getOperand(1));
  if (!Number && Op->isCommutative())
    Number = llvm::dyn_cast<llvm::ConstantInt>(Op->getOperand(0));
  if (!Number)
    return std::nullopt;
  const llvm::APInt &Bits = Number->getValue();
  switch (Op->getOpcode()) {
  case llvm::Instruction::And:
    if (Bits.isPowerOf2())
      return Pair(llvm::APInt(Width, 0), Bits);
    break;
  case llvm::Instruction::Or:
    if ((~Bits).isPowerOf2())
      return Pair(Bits, llvm::APInt::getAllOnes(Width));
    break;
  case llvm::Instruction::LShr:
    if (Bits == Width - 1)
      return Pair(llvm::APInt(Width, 0), llvm::APInt(Width, 1));
    break;
  case llvm::Instruction::AShr:
    if (Bits == Width - 1)
      return Pair(llvm::APInt(Width, 0), llvm::APInt::getAllOnes(Width));
    break;
  default:
    break;
  }
  return std::nullopt;
}

// Constant folding alone may legally refine poison. Here every enumerated
// operation must instead be defined: discharge each supported flag, reject
// unknown annotations, then ask LLVM's integer constant folder for the value.
bool definedForValues(const llvm::Instruction &I,
                      llvm::ArrayRef<llvm::APInt> Values) {
  using namespace llvm;
  const APInt &A = Values[0];
  const APInt &B = Values.size() > 1 ? Values[1] : A;
  Instruction *Copy = I.clone();
  auto Finish = [&](bool Defined) {
    Defined &= !Copy->hasPoisonGeneratingAnnotations();
    Copy->deleteValue();
    return Defined;
  };
  if (I.isShift() && B.uge(A.getBitWidth()))
    return Finish(false);
  if (const auto *Op = dyn_cast<OverflowingBinaryOperator>(&I)) {
    auto Overflow = [&](bool Signed) {
      bool Result = true;
      switch (I.getOpcode()) {
      case Instruction::Add:
        if (Signed)
          (void)A.sadd_ov(B, Result);
        else
          (void)A.uadd_ov(B, Result);
        break;
      case Instruction::Sub:
        if (Signed)
          (void)A.ssub_ov(B, Result);
        else
          (void)A.usub_ov(B, Result);
        break;
      case Instruction::Mul:
        if (Signed)
          (void)A.smul_ov(B, Result);
        else
          (void)A.umul_ov(B, Result);
        break;
      case Instruction::Shl:
        if (Signed)
          (void)A.sshl_ov(B, Result);
        else
          (void)A.ushl_ov(B, Result);
        break;
      default:
        break;
      }
      return Result;
    };
    if ((Op->hasNoUnsignedWrap() && Overflow(false)) ||
        (Op->hasNoSignedWrap() && Overflow(true)))
      return Finish(false);
    Copy->setHasNoUnsignedWrap(false);
    Copy->setHasNoSignedWrap(false);
  }
  if (const auto *Op = dyn_cast<PossiblyExactOperator>(&I)) {
    if (Op->isExact() &&
        !(A & APInt::getLowBitsSet(A.getBitWidth(), B.getZExtValue())).isZero())
      return Finish(false);
    Copy->setIsExact(false);
  }
  if (const auto *Op = dyn_cast<PossiblyDisjointInst>(&I)) {
    if (Op->isDisjoint() && !(A & B).isZero())
      return Finish(false);
    cast<PossiblyDisjointInst>(Copy)->setIsDisjoint(false);
  }
  if (const auto *Op = dyn_cast<TruncInst>(&I)) {
    const unsigned Width = I.getType()->getIntegerBitWidth();
    if ((Op->hasNoUnsignedWrap() && A.getActiveBits() > Width) ||
        (Op->hasNoSignedWrap() && !A.isSignedIntN(Width)))
      return Finish(false);
    cast<TruncInst>(Copy)->setHasNoUnsignedWrap(false);
    cast<TruncInst>(Copy)->setHasNoSignedWrap(false);
  }
  if (isa<ZExtInst>(I)) {
    if (I.hasNonNeg() && A.isNegative())
      return Finish(false);
    Copy->setNonNeg(false);
  }
  if (const auto *Op = dyn_cast<ICmpInst>(&I)) {
    if (Op->hasSameSign() && A.isNegative() != B.isNegative())
      return Finish(false);
    cast<ICmpInst>(Copy)->setSameSign(false);
  }
  return Finish(true);
}

class SliceAnalysis {
  const llvm::DataLayout &DL;
  WorkBudget &Budget;
  llvm::DenseMap<llvm::Value *, std::optional<TwoValues>> Memo;
  llvm::DenseSet<llvm::Value *> Undefined;
  llvm::DenseMap<llvm::Value *, bool> AnchorSafety;

  std::optional<TwoValues> seed(llvm::Value *V) {
    auto Result = anchorValues(V);
    if (!Result)
      return std::nullopt;
    if (auto It = AnchorSafety.find(V); It != AnchorSafety.end())
      return It->second ? Result : std::nullopt;
    // A repeated explicit undef need not denote the same value at each use.
    // Do not hide it behind a PHI or an otherwise opaque arithmetic boundary.
    // A freeze supplies its own stable identity; memory/call results remain
    // observations of their original instructions, not their operand graphs.
    llvm::SmallVector<llvm::Value *, 32> Work{V};
    llvm::DenseSet<llvm::Value *> Seen;
    while (!Work.empty()) {
      if (!Budget.spend())
        return std::nullopt;
      auto *Current = Work.pop_back_val();
      if (!Seen.insert(Current).second)
        continue;
      if (llvm::isa<llvm::UndefValue, llvm::PoisonValue>(Current)) {
        AnchorSafety[V] = false;
        return std::nullopt;
      }
      auto *I = llvm::dyn_cast<llvm::Instruction>(Current);
      if (!I || llvm::isa<llvm::LoadInst, llvm::CallBase, llvm::FreezeInst>(I))
        continue;
      for (llvm::Value *Op : I->operands()) {
        if (!Budget.spend())
          return std::nullopt;
        Work.push_back(Op);
      }
    }
    AnchorSafety[V] = true;
    return Result;
  }

  std::optional<TwoValues> derive(llvm::Instruction &I) {
    llvm::SmallVector<TwoValues, 3> Args;
    llvm::Value *Anchor = nullptr;
    llvm::APInt Base(1, 0);
    for (llvm::Value *Op : I.operands()) {
      const auto P = Memo.lookup(Op);
      if (!P || (Anchor && P->Anchor && Anchor != P->Anchor))
        return std::nullopt;
      if (P->Anchor) {
        Anchor = P->Anchor;
        Base = P->AnchorFirst;
      }
      Args.push_back(*P);
    }
    if (!Anchor)
      return std::nullopt;
    llvm::SmallVector<llvm::APInt, 2> Results;
    for (unsigned Side = 0; Side != 2; ++Side) {
      llvm::SmallVector<llvm::APInt, 3> Values;
      llvm::SmallVector<llvm::Constant *, 3> Constants;
      for (const TwoValues &Arg : Args) {
        const auto &V = Side ? Arg.Second : Arg.First;
        if (!Budget.spend(1 + V.getBitWidth() / 64))
          return std::nullopt;
        Values.push_back(V);
        Constants.push_back(llvm::ConstantInt::get(I.getContext(), V));
      }
      if (!definedForValues(I, Values))
        return std::nullopt;
      const auto *Result = llvm::dyn_cast_or_null<llvm::ConstantInt>(
          llvm::ConstantFoldInstOperands(&I, Constants, DL, nullptr, false));
      if (!Result)
        return std::nullopt;
      Results.push_back(Result->getValue());
    }
    return TwoValues{Anchor, Base, Results[0], Results[1]};
  }

public:
  SliceAnalysis(const llvm::DataLayout &DL, WorkBudget &Budget)
      : DL(DL), Budget(Budget) {}
  void clear() {
    Memo.clear();
    Undefined.clear();
    AnchorSafety.clear();
  }

  std::optional<TwoValues> get(llvm::Value *Root) {
    struct Item {
      llvm::Value *V;
      bool Ready;
    };
    llvm::SmallVector<Item, 32> Work{{Root, false}};
    llvm::DenseSet<llvm::Value *> Active;
    while (!Work.empty()) {
      if (!Budget.spend())
        return std::nullopt;
      auto [V, Ready] = Work.pop_back_val();
      if (Memo.contains(V))
        continue;
      if (!supportedType(V->getType()) ||
          llvm::isa<llvm::UndefValue, llvm::PoisonValue>(V)) {
        Memo[V] = std::nullopt;
        Undefined.insert(V);
        continue;
      }
      if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V)) {
        Memo[V] =
            TwoValues{nullptr, llvm::APInt(1, 0), C->getValue(), C->getValue()};
        continue;
      }
      auto *I = llvm::dyn_cast<llvm::Instruction>(V);
      if (!I || !supportedInstruction(*I)) {
        Memo[V] = seed(V);
        continue;
      }
      if (!Ready) {
        if (!Active.insert(V).second)
          return std::nullopt;
        Work.push_back({V, true});
        for (llvm::Value *Op : I->operands()) {
          if (!Budget.spend())
            return std::nullopt;
          if (!Memo.contains(Op))
            Work.push_back({Op, false});
        }
        continue;
      }
      Active.erase(V);
      if (llvm::any_of(I->operands(), [&](const llvm::Use &U) {
            return Undefined.contains(U.get());
          })) {
        Memo[V] = std::nullopt;
        Undefined.insert(V);
        continue;
      }
      auto Result = derive(*I);
      Memo[V] = Result ? Result : seed(V);
    }
    return Memo.lookup(Root);
  }
};

unsigned removableInstructions(llvm::Instruction *Root, llvm::Value *Anchor,
                               WorkBudget &Budget) {
  llvm::DenseMap<llvm::Value *, unsigned> Uses;
  llvm::SmallVector<llvm::Value *, 32> Work{Root};
  while (!Work.empty()) {
    if (!Budget.spend())
      return 0;
    auto *V = Work.pop_back_val();
    if (V == Anchor || Uses.contains(V))
      continue;
    const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
    if (!I || !supportedInstruction(*I))
      continue;
    Uses[V] = V->getNumUses();
    for (llvm::Value *Op : I->operands()) {
      if (!Budget.spend())
        return 0;
      Work.push_back(Op);
    }
  }
  Work.push_back(Root);
  unsigned Count = 0;
  while (!Work.empty()) {
    if (!Budget.spend())
      return 0;
    auto *V = Work.pop_back_val();
    const auto *I = llvm::dyn_cast<llvm::Instruction>(V);
    if (!I || !Uses.contains(V))
      continue;
    ++Count;
    for (llvm::Value *Op : I->operands()) {
      if (!Budget.spend())
        return 0;
      auto It = Uses.find(Op);
      if (It != Uses.end() && It->second && --It->second == 0)
        Work.push_back(Op);
    }
  }
  return Count;
}

} // namespace

unsigned simplifyFiniteValueSlices(llvm::Function &F,
                                   const SymSimplifyOptions &Opts) {
  WorkBudget Budget(Opts.MaxFiniteValueWork);
  if (Budget.exhausted())
    return 0;
  llvm::SmallVector<llvm::WeakTrackingVH, 64> Roots;
  for (llvm::Instruction &I : llvm::instructions(F)) {
    if (!Budget.spend())
      return 0;
    if (supportedInstruction(I) && !I.use_empty())
      Roots.push_back(&I);
  }
  SliceAnalysis Analysis(F.getDataLayout(), Budget);
  unsigned Rewrites = 0;
  // Visit consumers first so a complete composed slice can disappear at once.
  // The enclosing semantic fixed point will revisit any newly exposed slice.
  for (llvm::WeakTrackingVH &Handle : llvm::reverse(Roots)) {
    if (Budget.exhausted())
      break;
    auto *Root = llvm::dyn_cast_or_null<llvm::Instruction>(Handle);
    if (!Root || Root->use_empty())
      continue;
    const auto P = Analysis.get(Root);
    // Keeping a nonconstant dependence also keeps the anchor's poison
    // dependence. Never erase an opaque input by publishing a constant table.
    if (!P || !P->Anchor || P->Anchor == Root || P->First == P->Second)
      continue;
    const unsigned Removable = removableInstructions(Root, P->Anchor, Budget);
    if (Budget.exhausted() || Removable < Opts.MinInstructionsSaved)
      continue;
    llvm::SmallVector<llvm::Instruction *, 4> Created;
    llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderCallbackInserter> B(
        F.getContext(), llvm::ConstantFolder(),
        llvm::IRBuilderCallbackInserter(
            [&](llvm::Instruction *I) { Created.push_back(I); }));
    B.SetInsertPoint(Root);
    B.SetCurrentDebugLocation(Root->getDebugLoc());
    auto *First = llvm::ConstantInt::get(F.getContext(), P->AnchorFirst);
    llvm::Value *After;
    if (Root->getType()->isIntegerTy(1)) {
      After = B.CreateICmp(P->First.isOne() ? llvm::CmpInst::ICMP_EQ
                                            : llvm::CmpInst::ICMP_NE,
                           P->Anchor, First);
    } else {
      auto *Cond = B.CreateICmpEQ(P->Anchor, First);
      After =
          B.CreateSelect(Cond, llvm::ConstantInt::get(F.getContext(), P->First),
                         llvm::ConstantInt::get(F.getContext(), P->Second));
    }
    if (Created.size() > Removable - Opts.MinInstructionsSaved ||
        (Created.size() == Removable && !Opts.MinInstructionsSaved)) {
      for (auto *I : llvm::reverse(Created))
        I->eraseFromParent();
      continue;
    }
    Root->replaceAllUsesWith(After);
    Analysis.clear();
    llvm::RecursivelyDeleteTriviallyDeadInstructions(Root);
    ++Rewrites;
  }
  return Rewrites;
}

} // namespace neverd

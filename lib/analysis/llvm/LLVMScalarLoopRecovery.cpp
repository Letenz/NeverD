//===- LLVMScalarLoopRecovery.cpp - Scalar loop search --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryInternal.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

namespace neverd::analysis::scalar_recovery {
using namespace llvm;

bool Search::charge(uint64_t Amount) {
  if (Exhausted ||
      Amount > Limits.MaxConstructionWork - Result.ConstructionWork) {
    Exhausted = true;
    return false;
  }
  Result.ConstructionWork += Amount;
  return true;
}

static uint64_t size(const Function &F) {
  uint64_t N = F.arg_size() + F.size();
  for (const auto &B : F)
    for (const auto &I : B)
      N += 1 + I.getNumOperands();
  return N;
}

namespace {
class IntegerWidthMapper : public ValueMapTypeRemapper {
  unsigned MaxWidth;

public:
  explicit IntegerWidthMapper(unsigned Width) : MaxWidth(Width) {}
  Type *remapType(Type *T) override {
    if (T->isIntegerTy() && T->getIntegerBitWidth() > MaxWidth)
      return IntegerType::get(T->getContext(), MaxWidth);
    return T;
  }
};
} // namespace

bool Search::clone(Function &F, Candidate &C, bool Probe,
                   unsigned MaxInternalWidth) {
  if (!Probe && Result.Candidates >= Limits.MaxCandidates)
    Exhausted = true;
  if (!charge(size(F)))
    return false;
  C.Module = std::make_unique<Module>("scalar.loop.recovery", F.getContext());
  C.Module->setDataLayout(F.getParent()->getDataLayout());
  C.Module->setTargetTriple(F.getParent()->getTargetTriple());
  C.Function = Function::Create(F.getFunctionType(), F.getLinkage(),
                                F.getName(), C.Module.get());
  C.Function->copyAttributesFrom(&F);
  C.Values[&F] = C.Function;
  auto Destination = C.Function->arg_begin();
  for (auto &A : F.args()) {
    Destination->setName(A.getName());
    C.Values[&A] = &*Destination++;
  }
  // Admission has excluded globals, pointers and ordinary calls. Clone only
  // referenced intrinsic declarations; unrelated module contents stay put.
  for (auto &B : F)
    for (auto &I : B)
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        auto *Callee = Call->getCalledFunction();
        if (C.Values.count(Callee))
          continue;
        auto *Copy =
            Function::Create(Callee->getFunctionType(), Callee->getLinkage(),
                             Callee->getName(), C.Module.get());
        Copy->copyAttributesFrom(Callee);
        C.Values[Callee] = Copy;
      }
  if (MaxInternalWidth) {
    if (!charge(size(F)))
      return false;
    // A type remapper alone does not preserve narrowed nonzero literals.
    // Bind their original bit patterns explicitly before cloning operands.
    for (auto &B : F)
      for (auto &I : B)
        for (auto &U : I.operands())
          if (auto *K = dyn_cast<ConstantInt>(U.get()))
            if (K->getBitWidth() > MaxInternalWidth)
              C.Values[K] = ConstantInt::get(
                  F.getContext(), K->getValue().trunc(MaxInternalWidth));
  }
  IntegerWidthMapper Types(MaxInternalWidth);
  SmallVector<ReturnInst *, 8> Returns;
  CloneFunctionInto(C.Function, &F, C.Values,
                    CloneFunctionChangeType::DifferentModule, Returns, "",
                    nullptr, MaxInternalWidth ? &Types : nullptr);
  if (MaxInternalWidth) {
    if (!charge(size(*C.Function)))
      return false;
    SmallVector<CastInst *, 16> Redundant;
    for (auto &B : *C.Function)
      for (auto &I : B)
        if (auto *Cast = dyn_cast<CastInst>(&I))
          if (Cast->getType() == Cast->getOperand(0)->getType())
            Redundant.push_back(Cast);
    for (auto *Cast : Redundant) {
      Cast->replaceAllUsesWith(Cast->getOperand(0));
      Cast->eraseFromParent();
    }
  }
  return true;
}

static void clean(Function &F) {
  removeUnreachableBlocks(F);
  SmallVector<WeakTrackingVH, 32> Instructions;
  for (auto &B : F)
    for (auto &I : B)
      Instructions.push_back(&I);
  for (auto &V : Instructions)
    if (auto *I = dyn_cast_or_null<Instruction>(V))
      RecursivelyDeleteTriviallyDeadInstructions(I);
}

static Cost cost(Function &F) {
  Cost C;
  DominatorTree DT(F);
  LoopInfo LI(DT);
  for (auto &B : F)
    C.Instructions += B.size();
  for (auto *L : LI.getLoopsInPreorder()) {
    C.Carriers += std::distance(L->getHeader()->phis().begin(),
                                L->getHeader()->phis().end());
    SmallVector<BasicBlock *, 4> Exiting;
    L->getExitingBlocks(Exiting);
    for (auto *B : Exiting) {
      // A one-block loop tests after its update despite exiting in the header.
      auto *Branch = dyn_cast<CondBrInst>(B->getTerminator());
      auto *Compare =
          Branch ? dyn_cast<ICmpInst>(Branch->getCondition()) : nullptr;
      auto Counter = Compare ? counter(*L, *Compare) : std::nullopt;
      C.BottomTests += B != L->getHeader() || (Counter && Counter->CompareNext);
    }
  }
  return C;
}

static void maskProbeData(Function &F, ArrayRef<LLVMScalarControlBit> Bits) {
  for (auto &A : F.args()) {
    APInt Mask(A.getType()->getIntegerBitWidth(), 0);
    for (auto Bit : Bits)
      if (Bit.Argument == A.getArgNo())
        Mask.setBit(Bit.Bit);
    if (Mask.isZero()) {
      A.replaceAllUsesWith(ConstantInt::get(A.getType(), 0));
    } else if (!Mask.isAllOnes()) {
      IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
      auto *Value = B.CreateAnd(&A, ConstantInt::get(F.getContext(), Mask),
                                "recovery.probe");
      A.replaceUsesWithIf(Value, [&](Use &U) { return U.getUser() != Value; });
    }
  }
}

bool Search::screen(Candidate &C, Cost *CandidateCost) {
  if (stopped() || !charge(size(*C.Function)))
    return false;
  ++Result.Candidates;
  // Structural changes can fail dominance/PHI validation. They are proposals,
  // never permission to guess an unavailable seed or erase definedness.
  if (verifyFunction(*C.Function))
    return false;
  clean(*C.Function);
  if (verifyFunction(*C.Function))
    return false;
  auto NewCost = cost(*C.Function);
  if (!(NewCost < CurrentCost))
    return false;
  if (CandidateCost)
    *CandidateCost = NewCost;
  // A zero-data query across the complete source control domain is only a
  // rejection filter. Bad region guesses can otherwise grow symbolic data
  // for thousands of iterations before their first counterexample. All input
  // bits remain untouched in the subsequent mandatory symbolic proof.
  Candidate Probe;
  if (!clone(*C.Function, Probe, true))
    return false;
  maskProbeData(*Probe.Function, SourceControlBits);
  auto ProbeLimits = Limits.Proof;
  ProbeLimits.MaxWork =
      std::min(ProbeLimits.MaxWork, Limits.MaxProofWork - Result.ProofWork);
  auto Screen =
      checkLLVMScalarEquivalence(*ProbeSource->getFunction(Original.getName()),
                                 *Probe.Function, ProbeLimits);
  Result.ProofWork += Screen.Work;
  if (Screen.Status != LLVMScalarEquivalenceStatus::Proved) {
    Exhausted |=
        Screen.WorkLimitExceeded && Result.ProofWork == Limits.MaxProofWork;
    return false;
  }
  return true;
}

bool Search::accept(Candidate &C) {
  Cost NewCost;
  if (!screen(C, &NewCost))
    return false;
  auto ProofLimits = Limits.Proof;
  ProofLimits.MaxControlBits = ControlBitLimit;
  ProofLimits.MaxWork =
      std::min(ProofLimits.MaxWork, Limits.MaxProofWork - Result.ProofWork);
  if (!ProofLimits.MaxWork) {
    Exhausted = true;
    return false;
  }
  auto Proof = checkLLVMScalarEquivalence(Original, *C.Function, ProofLimits);
  Result.ProofWork += Proof.Work;
  if (Proof.Status != LLVMScalarEquivalenceStatus::Proved) {
    // A candidate may exceed its own path/node/query ceiling. It authorizes
    // no change, but other proposals can still fit the shared search budget.
    Exhausted |=
        Proof.WorkLimitExceeded && Result.ProofWork == Limits.MaxProofWork;
    return false;
  }
  if (!charge(DeferredSeeds.size()))
    return false;
  SmallPtrSet<PHINode *, 16> MappedSeeds;
  for (auto *P : DeferredSeeds)
    if (auto *Mapped = dyn_cast_or_null<PHINode>(C.map(P)))
      MappedSeeds.insert(Mapped);
  DeferredSeeds = std::move(MappedSeeds);
  Accepted = std::move(C.Module);
  CurrentCost = NewCost;
  return true;
}

LLVMScalarLoopRecoveryResult Search::run() {
  auto Model = modelLLVMScalarFunction(Original, Limits.Proof.Model);
  if (!Model) {
    Result.Status = LLVMScalarLoopRecoveryStatus::Unsupported;
    Result.Diagnostic = toString(Model.takeError());
    return std::move(Result);
  }
  if (!Limits.MaxCandidates || !Limits.MaxTransforms || !Limits.MaxProofWork ||
      !Limits.Proof.MaxWork)
    Exhausted = true;
  if (!Exhausted) {
    auto SourceLimits = Limits.Proof;
    SourceLimits.MaxWork = std::min(SourceLimits.MaxWork, Limits.MaxProofWork);
    auto Source = checkLLVMScalarEquivalence(Original, Original, SourceLimits);
    Result.ProofWork += Source.Work;
    if (Source.Status != LLVMScalarEquivalenceStatus::Proved) {
      Result.Status =
          Source.Status == LLVMScalarEquivalenceStatus::BudgetExceeded
              ? LLVMScalarLoopRecoveryStatus::BudgetExceeded
              : LLVMScalarLoopRecoveryStatus::Unsupported;
      Result.Diagnostic = Source.Diagnostic;
      return std::move(Result);
    }
    // This search preserves the source's complete control domain. A proposal
    // requiring additional input bits is refused by the existing checker,
    // never proved using an incomplete subset of those bits.
    ControlBitLimit = Source.ControlBits.size();
    SourceControlBits = std::move(Source.ControlBits);
  }
  Candidate Initial;
  // Cloning reads the function; it does not mutate even when its API accepts
  // a non-const source for mapping LLVM values.
  if (clone(const_cast<Function &>(Original), Initial)) {
    Current = std::move(Initial.Module);
    CurrentCost = cost(*Initial.Function);
  }
  if (!stopped()) {
    Candidate Probe;
    if (clone(const_cast<Function &>(Original), Probe, true)) {
      maskProbeData(*Probe.Function, SourceControlBits);
      ProbeSource = std::move(Probe.Module);
    }
  }
  for (unsigned Phase = 0; Phase < 6 && !stopped(); ++Phase) {
    while (!stopped()) {
      if (Result.ProvedTransforms >= Limits.MaxTransforms) {
        Exhausted = true;
        break;
      }
      auto &F = *Current->getFunction(Original.getName());
      if (!charge(size(F)))
        break;
      bool Changed = false;
      {
        DominatorTree DT(F);
        LoopInfo LI(DT);
        switch (Phase) {
        case 0:
          Changed = unpeel(*this, F, DT, LI);
          break;
        case 1:
          Changed = zeroTrip(*this, F, DT, LI);
          break;
        case 2:
          Changed = rotate(*this, F, LI);
          break;
        case 3:
          Changed = affine(*this, F, LI);
          break;
        case 4:
          Changed = seeds(*this, F, DT, LI);
          break;
        case 5:
          Changed = widths(*this, F, LI);
          break;
        }
      }
      if (!Changed)
        break;
      // Destroy the old function only after its dominator/loop analyses and
      // all proposal iterators have gone out of scope.
      Current = std::move(Accepted);
      ++Result.ProvedTransforms;
    }
  }
  if (Exhausted) {
    Result.Status = LLVMScalarLoopRecoveryStatus::BudgetExceeded;
    Result.Diagnostic = "scalar loop reconstruction budget exhausted";
  } else if (Result.ProvedTransforms) {
    Result.Status = LLVMScalarLoopRecoveryStatus::Recovered;
    Result.Module = std::move(Current);
  }
  return std::move(Result);
}

std::optional<Counter> counter(Loop &L, ICmpInst &Compare) {
  auto *Pre = L.getLoopPredecessor(), *Latch = L.getLoopLatch();
  if (!Pre || !Latch)
    return {};
  std::optional<Counter> C;
  for (auto &P : L.getHeader()->phis()) {
    if (P.getNumIncomingValues() != 2)
      continue;
    auto *Next = P.getIncomingValueForBlock(Latch);
    auto *Update = dyn_cast<BinaryOperator>(Next);
    if (!Update || Update->getOpcode() != Instruction::Add)
      continue;
    Value *A = Update->getOperand(0), *B = Update->getOperand(1);
    if (B == &P)
      std::swap(A, B);
    auto *Step = dyn_cast<ConstantInt>(B);
    if (A != &P || !Step || Step->isZero())
      continue;
    for (unsigned Side = 0; Side < 2; ++Side) {
      auto *V = Compare.getOperand(Side), *Other = Compare.getOperand(1 - Side);
      if ((V != &P && V != Next) || !L.isLoopInvariant(Other))
        continue;
      if (C)
        return {};
      C = Counter{&P, Step, Other, V == Next};
    }
  }
  return C;
}
} // namespace neverd::analysis::scalar_recovery

namespace neverd::analysis {
LLVMScalarLoopRecoveryResult
recoverLLVMScalarLoops(const llvm::Function &Function,
                       const LLVMScalarLoopRecoveryLimits &Limits) {
  return scalar_recovery::Search(Function, Limits).run();
}
} // namespace neverd::analysis

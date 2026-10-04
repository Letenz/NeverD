//===- LLVMScalarLoopRecoveryInternal.h - Loop proposals --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVM_SCALAR_LOOP_RECOVERY_INTERNAL_H
#define NEVERD_ANALYSIS_LLVM_SCALAR_LOOP_RECOVERY_INTERNAL_H

#include "neverd/analysis/LLVMScalarLoopRecovery.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <optional>

namespace neverd::analysis::scalar_recovery {

struct Candidate {
  llvm::ValueToValueMapTy Values;
  std::unique_ptr<llvm::Module> Module;
  llvm::Function *Function = nullptr;
  llvm::Value *map(llvm::Value *V) const {
    if (llvm::Value *Mapped = Values.lookup(V))
      return Mapped;
    return llvm::isa<llvm::Constant>(V) ? V : nullptr;
  }
  template <typename T> T *get(T *V) const { return llvm::cast<T>(map(V)); }
};

// Each accepted candidate reduces the tuple (non-header exit tests, loop
// carriers, live instructions). Extra blocks alone do not improve this cost.
struct Cost {
  unsigned BottomTests = 0;
  unsigned Carriers = 0;
  uint64_t Instructions = 0;
  auto operator<=>(const Cost &) const = default;
};

class Search {
  const llvm::Function &Original;
  const LLVMScalarLoopRecoveryLimits &Limits;
  LLVMScalarLoopRecoveryResult Result;
  std::unique_ptr<llvm::Module> Current;
  std::unique_ptr<llvm::Module> Accepted;
  std::unique_ptr<llvm::Module> ProbeSource;
  Cost CurrentCost;
  unsigned ControlBitLimit = 0;
  std::vector<LLVMScalarControlBit> SourceControlBits;
  bool Exhausted = false;
  // Discovery scheduling only: defer previously tried seed replacements
  // until a later search. Never reuse a rejection as a semantic fact.
  llvm::SmallPtrSet<llvm::PHINode *, 16> DeferredSeeds;

public:
  Search(const llvm::Function &F, const LLVMScalarLoopRecoveryLimits &L)
      : Original(F), Limits(L) {}
  bool charge(uint64_t Amount = 1);
  bool stopped() const { return Exhausted; }
  bool clone(llvm::Function &F, Candidate &C, bool Probe = false,
             unsigned MaxInternalWidth = 0);
  // Rejection only; success here cannot authorize publication.
  bool screen(Candidate &C, Cost *CandidateCost = nullptr);
  bool accept(Candidate &C);
  bool seedDeferred(llvm::PHINode &P) const {
    return DeferredSeeds.contains(&P);
  }
  bool deferSeed(llvm::PHINode &P) {
    return charge() && DeferredSeeds.insert(&P).second;
  }
  LLVMScalarLoopRecoveryResult run();
};

bool unpeel(Search &S, llvm::Function &F, llvm::DominatorTree &DT,
            llvm::LoopInfo &LI);
bool zeroTrip(Search &S, llvm::Function &F, llvm::DominatorTree &DT,
              llvm::LoopInfo &LI);
bool rotate(Search &S, llvm::Function &F, llvm::LoopInfo &LI);
bool affine(Search &S, llvm::Function &F, llvm::LoopInfo &LI);
bool seeds(Search &S, llvm::Function &F, llvm::DominatorTree &DT,
           llvm::LoopInfo &LI);
bool widths(Search &S, llvm::Function &F, llvm::LoopInfo &LI);

struct Counter {
  llvm::PHINode *Phi = nullptr;
  llvm::ConstantInt *Step = nullptr;
  llvm::Value *Bound = nullptr;
  bool CompareNext = false;
};
std::optional<Counter> counter(llvm::Loop &L, llvm::ICmpInst &Compare);

} // namespace neverd::analysis::scalar_recovery
#endif

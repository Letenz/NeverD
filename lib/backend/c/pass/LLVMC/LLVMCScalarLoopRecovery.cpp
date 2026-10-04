//===- LLVMCScalarLoopRecovery.cpp - Proved scalar source loops -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMCScalarLoopRecovery.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::llvmc {
namespace {
using namespace llvm;

// This only bounds and filters discovery. The shared scalar importer owns
// admission, definedness and the input contract; this scan authorizes nothing.
bool hasScalarLoop(Function &F, uint64_t &ScanWork) {
  if (F.isDeclaration() || F.isVarArg() || F.size() > 1024 ||
      !F.getReturnType()->isIntegerTy() || F.hasMetadata() ||
      F.hasPersonalityFn())
    return false;
  if (F.arg_size() > ScanWork) {
    ScanWork = 0;
    return false;
  }
  ScanWork -= F.arg_size();
  for (const auto &A : F.args())
    if (!A.getType()->isIntegerTy() || !A.hasAttribute(Attribute::NoUndef))
      return false;
  for (const auto &B : F) {
    // Scalar return equivalence does not authorize changing externally
    // retained block addresses when replacing the function's body.
    if (B.hasAddressTaken())
      return false;
    const uint64_t Size = 1 + B.size();
    if (Size > ScanWork) {
      ScanWork = 0;
      return false;
    }
    ScanWork -= Size;
  }
  DominatorTree DT(F);
  LoopInfo Loops(DT);
  return !Loops.empty();
}

bool cleanCandidate(Module &M) {
  LoopAnalysisManager Loops;
  FunctionAnalysisManager Functions;
  CGSCCAnalysisManager CallGraph;
  ModuleAnalysisManager Modules;
  PassBuilder Passes;
  Passes.registerModuleAnalyses(Modules);
  Passes.registerCGSCCAnalyses(CallGraph);
  Passes.registerFunctionAnalyses(Functions);
  Passes.registerLoopAnalyses(Loops);
  Passes.crossRegisterProxies(Loops, Functions, CallGraph, Modules);
  ModulePassManager Pipeline;
  if (auto Error = Passes.parsePassPipeline(Pipeline, "function(early-cse)")) {
    consumeError(std::move(Error));
    return false;
  }
  FunctionPassManager Combine;
  Combine.addPass(InstCombinePass(
      InstCombineOptions().setMaxIterations(32).setVerifyFixpoint(true)));
  Pipeline.addPass(createModuleToFunctionPassAdaptor(std::move(Combine)));
  if (auto Error = Passes.parsePassPipeline(
          Pipeline, "function(reassociate,gvn,sccp,adce,simplifycfg)")) {
    consumeError(std::move(Error));
    return false;
  }
  Pipeline.run(M, Modules);
  return !verifyModule(M);
}

bool publish(Function &Destination, Function &Candidate) {
  Module &M = *Destination.getParent();
  SmallVector<const Function *, 8> Missing;
  ValueToValueMapTy Values;
  if (Destination.getFunctionType() != Candidate.getFunctionType() ||
      Destination.getCallingConv() != Candidate.getCallingConv())
    return false;
  // Validate every symbol before adding declarations or deleting any body.
  // A module-local name collision cannot silently substitute another callee.
  for (const auto &F : *Candidate.getParent()) {
    if (&F == &Candidate)
      continue;
    if (!F.isDeclaration() || !F.isIntrinsic())
      return false;
    if (auto *Existing = M.getNamedValue(F.getName())) {
      auto *Callee = dyn_cast<Function>(Existing);
      if (!Callee || !Callee->isDeclaration() ||
          Callee->getFunctionType() != F.getFunctionType() ||
          Callee->getAttributes() != F.getAttributes() ||
          Callee->getCallingConv() != F.getCallingConv())
        return false;
      Values[&F] = Callee;
    } else {
      Missing.push_back(&F);
    }
  }
  for (const auto *F : Missing) {
    auto *Copy = Function::Create(F->getFunctionType(), F->getLinkage(),
                                  F->getName(), &M);
    Copy->copyAttributesFrom(F);
    Values[F] = Copy;
  }
  Values[&Candidate] = &Destination;
  auto Argument = Destination.arg_begin();
  for (auto &A : Candidate.args())
    Values[&A] = &*Argument++;
  const auto Linkage = Destination.getLinkage();
  const auto Attributes = Destination.getAttributes();
  Destination.deleteBody();
  SmallVector<ReturnInst *, 8> Returns;
  CloneFunctionInto(&Destination, &Candidate, Values,
                    CloneFunctionChangeType::DifferentModule, Returns);
  Destination.setLinkage(Linkage);
  Destination.setAttributes(Attributes);
  return true;
}
} // namespace

unsigned
recoverScalarLoops(llvm::Module &Module, const llvm::Function *Only,
                   const analysis::LLVMScalarLoopRecoveryLimits &Limits) {
  using namespace llvm;
  using namespace analysis;
  if (Module.getNamedMetadata(windows_eh_md::FunctionTable))
    return 0;
  auto Remaining = Limits;
  uint64_t ScanWork = std::min<uint64_t>(65536, Limits.MaxConstructionWork);
  SmallVector<Function *, 16> Functions;
  if (Only) {
    if (Only->getParent() != &Module)
      return 0;
    Functions.push_back(const_cast<Function *>(Only));
  } else {
    for (auto &F : Module) {
      if (!ScanWork)
        break;
      --ScanWork;
      Functions.push_back(&F);
    }
  }
  unsigned Changed = 0;
  for (auto *F : Functions) {
    if (!ScanWork || !Remaining.MaxConstructionWork ||
        !Remaining.MaxProofWork || !Remaining.MaxCandidates ||
        !Remaining.MaxTransforms)
      break;
    if (!hasScalarLoop(*F, ScanWork))
      continue;
    std::unique_ptr<llvm::Module> Recovered;
    const Function *Input = F;
    bool Refused = false;
    // Cleanup can expose a shared zero-trip exit after prefix recovery. Give
    // the shared search another opportunity without resetting its budgets.
    // Never publish an intermediate body if a later search is exhausted.
    while (true) {
      if (!Remaining.MaxConstructionWork || !Remaining.MaxProofWork ||
          !Remaining.MaxCandidates || !Remaining.MaxTransforms) {
        Refused = true;
        break;
      }
      auto R = recoverLLVMScalarLoops(*Input, Remaining);
      Remaining.MaxConstructionWork -= R.ConstructionWork;
      Remaining.MaxProofWork -= R.ProofWork;
      Remaining.MaxCandidates -= R.Candidates;
      Remaining.MaxTransforms -= R.ProvedTransforms;
      if (R.Status == LLVMScalarLoopRecoveryStatus::BudgetExceeded) {
        Refused = true;
        break;
      }
      if (R.Status != LLVMScalarLoopRecoveryStatus::Recovered)
        break;
      if (!cleanCandidate(*R.Module)) {
        Refused = true;
        break;
      }
      Recovered = std::move(R.Module);
      Input = Recovered->getFunction(F->getName());
      if (!Input) {
        Refused = true;
        break;
      }
    }
    if (Refused || !Recovered || !Remaining.MaxProofWork)
      continue;
    auto *Candidate = Recovered->getFunction(F->getName());
    if (!Candidate)
      continue;
    // Cleanup may change shape or definedness. Prove the exact body that will
    // be published, against the complete original function, not just against
    // a partially accepted search state.
    auto ProofLimits = Limits.Proof;
    ProofLimits.MaxWork = std::min(ProofLimits.MaxWork, Remaining.MaxProofWork);
    auto Proof = checkLLVMScalarEquivalence(*F, *Candidate, ProofLimits);
    Remaining.MaxProofWork -= Proof.Work;
    if (Proof.Status == LLVMScalarEquivalenceStatus::Proved &&
        publish(*F, *Candidate))
      ++Changed;
  }
  return Changed;
}

} // namespace neverd::llvmc

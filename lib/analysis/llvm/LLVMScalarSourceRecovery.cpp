//===- LLVMScalarSourceRecovery.cpp - Proved scalar preparation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/analysis/LLVMScalarSourceRecovery.h"

#include "LLVMScalarLoopRecoveryInternal.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"

namespace neverd::analysis {
namespace {
using namespace llvm;

enum class CleanupStatus { Ready, Unsupported, BudgetExceeded };

CleanupStatus cleanCandidate(Module &M,
                             LLVMScalarLoopRecoveryLimits &Remaining) {
  if (!Remaining.MaxConstructionWork)
    return CleanupStatus::BudgetExceeded;
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
    return CleanupStatus::Unsupported;
  }
  FunctionPassManager Combine;
  Combine.addPass(InstCombinePass(
      InstCombineOptions().setMaxIterations(32).setVerifyFixpoint(false)));
  Pipeline.addPass(createModuleToFunctionPassAdaptor(std::move(Combine)));
  if (auto Error = Passes.parsePassPipeline(
          Pipeline, "function(reassociate,gvn,sccp,adce,simplifycfg)")) {
    consumeError(std::move(Error));
    return CleanupStatus::Unsupported;
  }
  Pipeline.run(M, Modules);
  bool Changed = false;
  for (auto &F : M) {
    if (!Remaining.MaxConstructionWork)
      return CleanupStatus::BudgetExceeded;
    --Remaining.MaxConstructionWork;
    if (F.isDeclaration())
      continue;
    SymSimplifyOptions Options;
    Options.MaxFiniteValueWork =
        std::min<uint64_t>(65536, Remaining.MaxConstructionWork);
    const auto Finite = SymSimplifyPass::simplifyFiniteValues(F, Options);
    Remaining.MaxConstructionWork -= Finite.Work;
    Options.MaxPredicateWork =
        std::min<uint64_t>(65536, Remaining.MaxConstructionWork);
    const auto R = SymSimplifyPass::simplifyPredicates(F, Options);
    Remaining.MaxConstructionWork -= R.Work;
    if (Finite.Rewrites || R.Rewrites) {
      Functions.invalidate(F, PreservedAnalyses::none());
      Changed = true;
    }
  }
  if (Changed) {
    // Recovery can expose a new predicate after the ordinary semantic pass.
    // InstCombine first reveals its scalar boundary; LICM then makes an
    // invariant boundary available outside the loop for the next search.
    // CSE gives equivalent counter updates one SSA identity again.
    // The final complete-source proof remains mandatory after all cleanup.
    ModulePassManager Finish;
    auto AddCombine = [&] {
      FunctionPassManager F;
      F.addPass(InstCombinePass(
          InstCombineOptions().setMaxIterations(32).setVerifyFixpoint(false)));
      Finish.addPass(createModuleToFunctionPassAdaptor(std::move(F)));
    };
    AddCombine();
    if (auto Error =
            Passes.parsePassPipeline(Finish, "function(loop-mssa(licm))")) {
      consumeError(std::move(Error));
      return CleanupStatus::Unsupported;
    }
    AddCombine();
    if (auto Error = Passes.parsePassPipeline(
            Finish, "function(early-cse,simplifycfg)")) {
      consumeError(std::move(Error));
      return CleanupStatus::Unsupported;
    }
    Finish.run(M, Modules);
  }
  return verifyModule(M) ? CleanupStatus::Unsupported : CleanupStatus::Ready;
}

} // namespace

LLVMScalarSourceRecoveryResult
recoverLLVMScalarSource(const llvm::Function &F,
                        const LLVMScalarSourceRecoveryLimits &Limits) {
  using namespace llvm;
  LLVMScalarSourceRecoveryResult Result;
  auto Remaining = Limits.Search;
  auto Finish = [&](LLVMScalarLoopRecoveryStatus Status, std::string Message) {
    Result.Status = Status;
    Result.Diagnostic = std::move(Message);
    Result.ConstructionWork =
        Limits.Search.MaxConstructionWork - Remaining.MaxConstructionWork;
    Result.ProofWork = Limits.Search.MaxProofWork - Remaining.MaxProofWork;
    Result.Candidates = Limits.Search.MaxCandidates - Remaining.MaxCandidates;
    Result.ProvedTransforms =
        Limits.Search.MaxTransforms - Remaining.MaxTransforms;
    return std::move(Result);
  };
  auto Exhausted = [&] {
    return Finish(LLVMScalarLoopRecoveryStatus::BudgetExceeded,
                  "scalar source recovery budget exhausted");
  };
  if (!Remaining.MaxConstructionWork || !Remaining.MaxProofWork ||
      !Remaining.Proof.MaxWork || !Remaining.MaxCandidates ||
      !Remaining.MaxTransforms || !Limits.MaxBoundaryProofWork ||
      !Limits.MaxCleanupRounds)
    return Exhausted();
  auto Model = modelLLVMScalarFunction(F, Remaining.Proof.Model);
  if (!Model)
    return Finish(LLVMScalarLoopRecoveryStatus::Unsupported,
                  toString(Model.takeError()));
  // Model admission bounds traversal and excludes external effects. Charge
  // the complete function before cloning, including dead source operations.
  uint64_t Size = F.arg_size() + F.size();
  for (const auto &B : F)
    for (const auto &I : B)
      Size += 1 + I.getNumOperands();
  if (Size > Remaining.MaxConstructionWork)
    return Exhausted();
  Remaining.MaxConstructionWork -= Size;
  scalar_recovery::Candidate Initial;
  scalar_recovery::cloneScalarFunction(F, Initial);
  auto Current = std::move(Initial.Module);
  auto Cleanup = [&](Module &M) {
    if (Result.CleanupRounds == Limits.MaxCleanupRounds)
      return CleanupStatus::BudgetExceeded;
    ++Result.CleanupRounds;
    return cleanCandidate(M, Remaining);
  };
  auto Prove = [&](const Function &Candidate) {
    auto ProofLimits = Remaining.Proof;
    ProofLimits.MaxWork =
        std::min(Limits.MaxBoundaryProofWork, Remaining.MaxProofWork);
    auto Proof = checkLLVMScalarEquivalence(F, Candidate, ProofLimits);
    Remaining.MaxProofWork -= Proof.Work;
    return Proof;
  };
  auto RefuseProof = [&](const LLVMScalarEquivalenceResult &Proof) {
    return Finish(Proof.Status == LLVMScalarEquivalenceStatus::BudgetExceeded
                      ? LLVMScalarLoopRecoveryStatus::BudgetExceeded
                      : LLVMScalarLoopRecoveryStatus::Unsupported,
                  Proof.Diagnostic);
  };
  auto RefuseCleanup = [&](CleanupStatus Status) {
    return Status == CleanupStatus::BudgetExceeded
               ? Exhausted()
               : Finish(LLVMScalarLoopRecoveryStatus::Unsupported,
                        "scalar cleanup could not construct a valid proposal");
  };
  auto Cleaned = Cleanup(*Current);
  if (Cleaned != CleanupStatus::Ready)
    return RefuseCleanup(Cleaned);
  auto Preparation = Prove(*Current->getFunction(F.getName()));
  if (Preparation.Status != LLVMScalarEquivalenceStatus::Proved)
    return RefuseProof(Preparation);
  bool NeedsFinalProof = false;
  while (true) {
    if (!Remaining.MaxConstructionWork || !Remaining.MaxProofWork ||
        !Remaining.MaxCandidates || !Remaining.MaxTransforms)
      return Exhausted();
    auto R =
        recoverLLVMScalarLoops(*Current->getFunction(F.getName()), Remaining);
    Remaining.MaxConstructionWork -= R.ConstructionWork;
    Remaining.MaxProofWork -= R.ProofWork;
    Remaining.MaxCandidates -= R.Candidates;
    Remaining.MaxTransforms -= R.ProvedTransforms;
    if (R.Status == LLVMScalarLoopRecoveryStatus::BudgetExceeded ||
        R.Status == LLVMScalarLoopRecoveryStatus::Unsupported)
      return Finish(R.Status, std::move(R.Diagnostic));
    if (R.Status == LLVMScalarLoopRecoveryStatus::Unchanged)
      break;
    Cleaned = Cleanup(*R.Module);
    if (Cleaned != CleanupStatus::Ready)
      return RefuseCleanup(Cleaned);
    Current = std::move(R.Module);
    NeedsFinalProof = true;
  }
  if (NeedsFinalProof) {
    // Intermediate cleanup may expose new loops, but it is never authority
    // to publish. Prove the exact final body against the complete original.
    auto Proof = Prove(*Current->getFunction(F.getName()));
    if (Proof.Status != LLVMScalarEquivalenceStatus::Proved)
      return RefuseProof(Proof);
  }
  Result.Module = std::move(Current);
  return Finish(LLVMScalarLoopRecoveryStatus::Recovered, {});
}

} // namespace neverd::analysis

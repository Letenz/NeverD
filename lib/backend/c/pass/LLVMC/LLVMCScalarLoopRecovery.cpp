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
                   const analysis::LLVMScalarSourceRecoveryLimits &Limits) {
  using namespace llvm;
  using namespace analysis;
  if (Module.getNamedMetadata(windows_eh_md::FunctionTable))
    return 0;
  auto Remaining = Limits;
  uint64_t ScanWork =
      std::min<uint64_t>(65536, Limits.Search.MaxConstructionWork);
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
    if (!ScanWork || !Remaining.Search.MaxConstructionWork ||
        !Remaining.Search.MaxProofWork || !Remaining.Search.MaxCandidates ||
        !Remaining.Search.MaxTransforms || !Remaining.MaxCleanupRounds)
      break;
    if (!hasScalarLoop(*F, ScanWork))
      continue;
    auto R = recoverLLVMScalarSource(*F, Remaining);
    Remaining.Search.MaxConstructionWork -= R.ConstructionWork;
    Remaining.Search.MaxProofWork -= R.ProofWork;
    Remaining.Search.MaxCandidates -= R.Candidates;
    Remaining.Search.MaxTransforms -= R.ProvedTransforms;
    Remaining.MaxCleanupRounds -= R.CleanupRounds;
    if (R.Status == LLVMScalarLoopRecoveryStatus::Recovered &&
        publish(*F, *R.Module->getFunction(F->getName())))
      ++Changed;
  }
  return Changed;
}

} // namespace neverd::llvmc

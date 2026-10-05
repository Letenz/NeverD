//===- LLVMScalarInputProjection.cpp - Retained scalar input demand
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/LLVMScalarInputProjection.h"

#include "neverd/analysis/LLVMScalarFunctionModel.h"

#include "llvm/IR/Verifier.h"

namespace neverd::analysis {
LLVMScalarInputProjectionResult
projectLLVMScalarInputs(const llvm::Function &Function,
                        const LLVMScalarInputProjectionLimits &Limits) {
  using namespace llvm;
  using Result = LLVMScalarInputProjectionResult;
  Result Output;
  auto Refuse = [&](Result::Kind Status, std::string Diagnostic) {
    Output.Status = Status;
    Output.Diagnostic = std::move(Diagnostic);
    return std::move(Output);
  };
  auto Exhausted = [&] {
    return Refuse(Result::BudgetExceeded,
                  "scalar input projection work budget exhausted");
  };
  if (!Limits.MaxConstructionWork)
    return Exhausted();
  if (!Function.getReturnType()->isIntegerTy() || Function.hasMetadata())
    return Refuse(Result::Unsupported,
                  "input projection requires an integer result without "
                  "function metadata");
  auto Prepared = projectLLVMScalarResult(
      Function, {{}, 0, Function.getReturnType()->getIntegerBitWidth()},
      Limits);
  Output.ConstructionWork = Prepared.ConstructionWork;
  if (Prepared.Status != LLVMScalarResultProjectionResult::Projected)
    return Refuse(Prepared.Status ==
                          LLVMScalarResultProjectionResult::BudgetExceeded
                      ? Result::BudgetExceeded
                      : Result::Unsupported,
                  std::move(Prepared.Diagnostic));
  if (Prepared.RemovedPackaging)
    return Refuse(Result::Unsupported,
                  "input projection cannot remove source packaging");
  auto Charge = [&](uint64_t Work = 1) {
    if (Work > Limits.MaxConstructionWork - Output.ConstructionWork)
      return false;
    Output.ConstructionWork += Work;
    return true;
  };
  llvm::Function *Source = nullptr;
  for (auto &F : *Prepared.Module) {
    if (!Charge())
      return Exhausted();
    if (!F.isDeclaration())
      Source = &F;
  }
  if (!Source)
    return Refuse(Result::Unsupported, "missing projected scalar definition");
  SmallVector<Type *, 16> Types;
  SmallVector<AttributeSet, 16> Attributes;
  std::vector<unsigned> Arguments;
  for (auto &A : Source->args()) {
    if (!Charge())
      return Exhausted();
    if (A.isUsedByMetadata())
      return Refuse(Result::Unsupported,
                    "input projection cannot rebind metadata arguments");
    if (A.use_empty())
      continue;
    // Charge every replacement use before changing the private clone.
    for (const auto &Use : A.uses()) {
      (void)Use;
      if (!Charge())
        return Exhausted();
    }
    if (!Charge(3))
      return Exhausted();
    Types.push_back(A.getType());
    Attributes.push_back(Source->getAttributes().getParamAttrs(A.getArgNo()));
    Arguments.push_back(A.getArgNo());
  }
  for (const auto &B : *Source) {
    if (!Charge())
      return Exhausted();
    for (const auto &I : B) {
      if (!Charge())
        return Exhausted();
      if (I.getDebugLoc() || I.hasDbgRecords())
        return Refuse(Result::Unsupported,
                      "input projection cannot rebind instruction debug info");
    }
  }
  if (!Charge(1 + Arguments.size()))
    return Exhausted();
  auto *Reduced = llvm::Function::Create(
      FunctionType::get(Source->getReturnType(), Types, false),
      Source->getLinkage(), "", *Prepared.Module);
  Reduced->copyAttributesFrom(Source);
  Reduced->setAttributes(AttributeList::get(
      Function.getContext(), Source->getAttributes().getFnAttrs(),
      Source->getAttributes().getRetAttrs(), Attributes));
  for (unsigned N = 0; N != Arguments.size(); ++N) {
    auto *Before = Source->getArg(Arguments[N]);
    auto *After = Reduced->getArg(N);
    After->setName(Before->getName());
    Before->replaceAllUsesWith(After);
  }
  // LLVM's attribute cloner expects mappings for all old parameters, even
  // unused ones. Move the already complete clone instead of inventing values
  // for omitted inputs or cloning with an incomplete argument map.
  Reduced->splice(Reduced->begin(), Source);
  Reduced->takeName(Source);
  Source->eraseFromParent();
  if (verifyModule(*Prepared.Module))
    return Refuse(Result::Unsupported, "invalid projected scalar interface");
  auto Model = modelLLVMScalarFunction(*Reduced, Limits.Model);
  if (!Model)
    return Refuse(Result::Unsupported, toString(Model.takeError()));
  Output.Status = Result::Projected;
  Output.RemovedArguments = Function.arg_size() - Arguments.size();
  Output.RetainedInstructions = Prepared.RetainedInstructions;
  Output.Arguments = std::move(Arguments);
  Output.Module = std::move(Prepared.Module);
  return Output;
}
} // namespace neverd::analysis

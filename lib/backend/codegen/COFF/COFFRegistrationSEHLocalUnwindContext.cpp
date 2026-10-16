//===- COFFRegistrationSEHLocalUnwindContext.cpp - Parent frame uses ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Identify complete native bridge operands before authenticating their slot.

#include "COFFRegistrationSEHLocalUnwind.h"

#include "neverd/Limits.h"

namespace neverd::coff_registration {

llvm::Expected<std::set<const llvm::LoadInst *>>
collectSEHLocalUnwindChainLoads(
    RegistrationLocalUnwindContract::Kind Protocol,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks) {
  std::set<const llvm::LoadInst *> Result;
  size_t Work = 0;
  for (const auto &[Key, Callback] : Callbacks)
    for (const auto &Block : *Callback)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("local unwind context exceeds its work budget");
        const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
        const auto *Bridge = Call ? Call->getCalledFunction() : nullptr;
        if (!Key.second || !Bridge || Bridge->isDeclaration())
          continue;
        auto Runtime = inspectSEHLocalUnwindBridge(*Bridge, Protocol);
        if (!Runtime) {
          llvm::consumeError(Runtime.takeError());
          continue;
        }
        if (Call->arg_size() != Bridge->arg_size() || Call->isTailCall() ||
            Call->getCallingConv() != llvm::CallingConv::C ||
            !Call->getAttributes().isEmpty() || Call->getNumOperandBundles())
          return rejectIR("local unwind context changed its call ABI");
        const auto *Pointer =
            llvm::dyn_cast<llvm::IntToPtrInst>(Call->getArgOperand(0));
        const auto *Load =
            Pointer ? llvm::dyn_cast<llvm::LoadInst>(Pointer->getOperand(0))
                    : nullptr;
        if (!Load || !Load->getType()->isIntegerTy(32) || !Load->isVolatile() ||
            Load->isAtomic() || Load->getAlign() != llvm::Align(4) ||
            !Load->hasOneUse() || !Pointer->hasOneUse() ||
            next(*Load) != Pointer || next(*Pointer) != Call ||
            Load->getParent() != &Block)
          return rejectIR("local unwind context lost its private chain read");
        Result.insert(Load);
      }
  return Result;
}

} // namespace neverd::coff_registration

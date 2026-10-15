//===- MedLLVMRegistrationSEHDynamicScope.cpp - Dynamic SEH intervals -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHDynamicScope.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/backend/llvm/X86RegistrationSEHScope.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"

namespace neverd::x86_registration {

void emitDynamicSEHScope(llvm::BasicBlock &Body, llvm::AllocaInst &Frame,
                         uint64_t EntrySP, va_t Owner, const MedBlock &Source,
                         llvm::ArrayRef<int32_t> Levels,
                         const std::map<int32_t, llvm::BasicBlock *> &Unwinds,
                         llvm::Function &TryBegin, llvm::Function &TryEnd) {
  using Role = windows_eh_md::NativeProvenanceRole;
  constexpr auto Model =
      windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
  auto &Context = Body.getContext();
  auto *Parent = Body.getParent();
  auto *After =
      Body.splitBasicBlock(Body.getTerminator(), "registration.dynamic.exit");
  auto *Entry = llvm::BasicBlock::Create(Context, "registration.dynamic.enter",
                                         Parent, &Body);
  Body.replaceAllUsesWith(Entry);
  llvm::IRBuilder<> B(Entry);
  med_llvm_eh::emitWindowsEHProvenanceAnchor(
      B, Model, Role::RangeEnterTarget, Owner, Source.StartAddr, Source.Id, 0,
      nullptr, Source.EndAddr, uint32_t(DynamicScopeLevel));
  B.CreateCall(&TryEnd);
  auto *State =
      B.CreateInBoundsGEP(B.getInt8Ty(), &Frame, B.getInt32(EntrySP - 8));
  auto *Level = B.CreateLoad(B.getInt32Ty(), State, "registration.scope");
  Level->setVolatile(true);
  Level->setAlignment(llvm::Align(1));
  const bool HasInactive =
      llvm::any_of(Levels, [](int32_t L) { return L < 0; });
  llvm::BasicBlock *Invalid = nullptr;
  if (!HasInactive) {
    Invalid =
        llvm::BasicBlock::Create(Context, "registration.invalid.scope", Parent);
    llvm::IRBuilder<>(Invalid).CreateUnreachable();
  }
  auto *SelectEntry = B.CreateSwitch(Level, HasInactive ? &Body : Invalid);
  Body.getTerminator()->eraseFromParent();
  llvm::IRBuilder<> Exit(&Body);
  auto *SelectExit = Exit.CreateSwitch(Level, HasInactive ? After : Invalid);
  for (int32_t Scope : Levels) {
    if (Scope < 0)
      continue;
    auto *Begin = llvm::BasicBlock::Create(Context, "registration.scope.enter",
                                           Parent, &Body);
    llvm::IRBuilder<> Before(Begin);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(Before, Model, Role::RangeEnter,
                                               Owner, Source.StartAddr, Scope,
                                               Source.Id);
    Before.CreateInvoke(&TryBegin, &Body, Unwinds.at(Scope));
    SelectEntry->addCase(B.getInt32(Scope), Begin);
    auto *End = llvm::BasicBlock::Create(Context, "registration.scope.exit",
                                         Parent, After);
    llvm::IRBuilder<> EndBuilder(End);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        EndBuilder, Model, Role::RangeExit, Owner, Source.EndAddr, Scope,
        Source.Id);
    EndBuilder.CreateInvoke(&TryEnd, After, Unwinds.at(Scope));
    SelectExit->addCase(B.getInt32(Scope), End);
  }
  llvm::IRBuilder<> AB(After->getTerminator());
  med_llvm_eh::emitWindowsEHProvenanceAnchor(
      AB, Model, Role::RangeExitTarget, Owner, Source.EndAddr, Source.Id, 0,
      nullptr, Source.StartAddr, uint32_t(DynamicScopeLevel));
}

} // namespace neverd::x86_registration

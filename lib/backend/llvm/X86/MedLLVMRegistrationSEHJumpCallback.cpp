//===- MedLLVMRegistrationSEHJumpCallback.cpp - Callback jump buffers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHJump.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace neverd::x86_registration {

std::map<llvm::CallInst *, SEHJumpCallbackCall>
emitSEHJumpCallbackBindings(const SEHJumpPlan &Plan, llvm::Function &Parent,
                            const std::map<va_t, llvm::AllocaInst *> &Buffers,
                            const std::map<va_t, llvm::Function *> &Callbacks) {
  std::map<llvm::CallInst *, SEHJumpCallbackCall> Result;
  std::map<va_t, unsigned> Indices;
  for (const auto &[Call, Source] : Plan.Calls)
    if (Source.Resume && Source.Resume->EntryVA)
      Indices.emplace(Source.Capture->Address, 0);
  if (Indices.empty())
    return Result;
  llvm::IntrinsicInst *Escape = nullptr;
  for (auto &I : Parent.getEntryBlock())
    if (auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        Intrinsic &&
        Intrinsic->getIntrinsicID() == llvm::Intrinsic::localescape)
      Escape = Intrinsic;
  assert(Escape && "outlined callbacks must already escape their source frame");
  std::vector<llvm::Value *> Slots;
  for (const auto &Argument : Escape->args())
    Slots.push_back(Argument.get());
  for (auto &[Address, Index] : Indices) {
    Index = Slots.size();
    Slots.push_back(Buffers.at(Address));
  }
  llvm::IRBuilder<> Entry(Escape);
  Entry.CreateCall(Escape->getCalledFunction(), Slots);
  Escape->eraseFromParent();

  std::map<std::pair<llvm::Function *, va_t>, llvm::Value *> Recovered;
  for (const auto &[Original, Source] : Plan.Calls) {
    if (!Source.Resume || !Source.Resume->EntryVA)
      continue;
    auto *Callback = Callbacks.at(Source.Resume->EntryVA);
    const auto Stack = getX86RegistrationSEHJumpStack(*Callback);
    assert(Stack && "outlined callback must retain its checked private stack");
    auto &Buffer = Recovered[{Callback, Source.Capture->Address}];
    if (!Buffer) {
      const auto *Frame =
          getX86RegistrationSEHJumpParentFrame(*Callback, Parent);
      assert(Frame && "outlining must establish the exact parent frame");
      llvm::IRBuilder<> B(Callback->getEntryBlock().getTerminator());
      Buffer =
          B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                           Parent.getParent(), llvm::Intrinsic::localrecover),
                       {&Parent, const_cast<llvm::Value *>(Frame),
                        B.getInt32(Indices.at(Source.Capture->Address))},
                       "registration.jump.recovered");
    }
    llvm::CallInst *Clone = nullptr;
    for (auto &Block : *Callback)
      for (auto &I : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            Call &&
            Call->getMetadata(windows_eh_md::RegistrationOperationAttachment) ==
                Original->getMetadata(
                    windows_eh_md::RegistrationOperationAttachment)) {
          assert(!Clone && "source call must have one callback clone");
          Clone = Call;
        }
    assert(Clone && "outlining must preserve every checked source call");
    Result.emplace(Original, SEHJumpCallbackCall{Clone, Buffer, *Stack});
  }
  return Result;
}

} // namespace neverd::x86_registration

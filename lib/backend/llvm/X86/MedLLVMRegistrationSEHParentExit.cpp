//===- MedLLVMRegistrationSEHParentExit.cpp - Parent context transfer -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Projects checked finally exits into a private native parent context.

#include "MedLLVMRegistrationSEHParentExit.h"

#include "MedLLVMRegistrationCopyBlocks.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace neverd::x86_registration {
namespace {

llvm::MDNode *marker(llvm::LLVMContext &Context, va_t Owner, uint64_t Identity,
                     unsigned Role) {
  auto Word = [&](uint64_t Value) {
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), Value));
  };
  return llvm::MDNode::get(Context, {Word(Owner), Word(Identity), Word(Role)});
}

void mark(llvm::Instruction &Instruction, va_t Owner, uint64_t Identity,
          unsigned Role) {
  Instruction.setMetadata(
      windows_eh_md::RegistrationParentExitAttachment,
      marker(Instruction.getContext(), Owner, Identity, Role));
}

} // namespace

std::optional<SEHParentExitPlan> prepareSEHParentExits(
    const MedFunc &Source,
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> *ChainIR) {
  auto Sources = getRegistrationSEHParentExits(Source);
  if (!Sources ||
      (!Sources->empty() &&
       !Source.RegistrationStates->SEHParentChainComplete) ||
      !hasCheckedRegistrationPHICopyEdges(OriginalBlocks, CopyEdges))
    return std::nullopt;
  SEHParentExitPlan Plan;
  Plan.Sources = std::move(*Sources);
  for (const auto &[Id, Exit] : Plan.Sources) {
    const auto SourceBlock = OriginalBlocks.find(Id);
    const auto TargetBlock = OriginalBlocks.find(Exit.TargetBlock);
    if (SourceBlock == OriginalBlocks.end() ||
        TargetBlock == OriginalBlocks.end())
      return std::nullopt;
    if (Exit.Operation->Opcode == NdOp::STORE) {
      const auto Identity =
          std::make_pair(Exit.Operation->Addr, Exit.Operation->OriginSeq);
      if (!ChainIR || !ChainIR->count(Identity))
        return std::nullopt;
      auto *Store =
          llvm::dyn_cast_or_null<llvm::StoreInst>(ChainIR->at(Identity));
      if (!Store || Store->getParent() != SourceBlock->second ||
          !llvm::isa<llvm::ConstantPointerNull>(Store->getPointerOperand()) ||
          Store->getPointerAddressSpace() != 257 ||
          !Store->getValueOperand()->getType()->isIntegerTy(32) ||
          Store->isAtomic() || Store->isVolatile() ||
          !Plan.ChainRemovals.emplace(Identity, Store).second)
        return std::nullopt;
    }
    auto *Branch = llvm::dyn_cast<llvm::UncondBrInst>(
        SourceBlock->second->getTerminator());
    if (!Branch)
      return std::nullopt;
    if (Branch->getSuccessor(0) != TargetBlock->second) {
      const auto Copy = CopyEdges.find(Branch->getSuccessor(0));
      if (Copy == CopyEdges.end() ||
          Copy->second != std::make_pair(Id, Exit.TargetBlock))
        return std::nullopt;
      Branch = llvm::cast<llvm::UncondBrInst>(Copy->first->getTerminator());
    }
    Plan.Branches.emplace(Id, Branch);
  }
  return Plan;
}

SEHParentExitProjection::SEHParentExitProjection(llvm::Function &Parent,
                                                 va_t Owner,
                                                 const SEHParentExitPlan &Plan)
    : Parent(Parent), Owner(Owner), Plan(Plan) {
  if (Plan.Sources.empty())
    return;
  // The private transfer restores the physical caller chain. A source
  // fallthrough's virtual FS removal is subsumed by that same checked write,
  // and must never execute on the runtime callback's current chain.
  for (const auto &[Identity, Store] : Plan.ChainRemovals) {
    DetachedRemovals.emplace(Store, Store->getNextNode());
    Store->removeFromParent();
  }
  llvm::IRBuilder<> Entry(&*Parent.getEntryBlock().getFirstInsertionPt());
  Context = Entry.CreateAlloca(llvm::ArrayType::get(Entry.getPtrTy(), 5),
                               nullptr, "registration.parent.context");
  Chain = Entry.CreateAlloca(Entry.getInt32Ty(), nullptr,
                             "registration.parent.chain");
  Destination = Entry.CreateAlloca(Entry.getInt32Ty(), nullptr,
                                   "registration.parent.destination");
  Context->setAlignment(llvm::Align(4));
  Chain->setAlignment(llvm::Align(4));
  Destination->setAlignment(llvm::Align(4));
  mark(*Context, Owner, 0, 0);
  mark(*Chain, Owner, 0, 1);
  mark(*Destination, Owner, 0, 2);
  auto *Module = Parent.getParent();
  const bool HasLongjmp = Module->getFunction("llvm.eh.sjlj.longjmp");
  auto *Longjmp = llvm::Intrinsic::getOrInsertDeclaration(
      Module, llvm::Intrinsic::eh_sjlj_longjmp);
  if (!HasLongjmp)
    CreatedLongjmp = Longjmp;
  for (const auto &[Id, Branch] : Plan.Branches) {
    auto *Transfer = llvm::BasicBlock::Create(
        Parent.getContext(), "registration.parent.transfer", &Parent);
    OriginalTargets.emplace(Id, Branch->getSuccessor(0));
    Branch->setSuccessor(0, Transfer);
    llvm::IRBuilder<> Builder(Transfer);
    auto *Select = Builder.CreateStore(Builder.getInt32(Id), Destination);
    Select->setVolatile(true);
    auto *Head = Builder.CreateLoad(Builder.getInt32Ty(), Chain);
    Head->setVolatile(true);
    auto *Restore = Builder.CreateStore(
        Head, llvm::ConstantPointerNull::get(Builder.getPtrTy(257)));
    Restore->setVolatile(true);
    auto *Call = Builder.CreateCall(Longjmp, {Context});
    Call->setDoesNotReturn();
    mark(*Call, Owner, Plan.Sources.at(Id).Operation->Addr, 3);
    Calls.emplace(Id, Call);
    Builder.CreateUnreachable();
  }
}

SEHParentExitProjection::~SEHParentExitProjection() {
  if (!Committed)
    rollback();
}

void SEHParentExitProjection::rollback() {
  if (Committed || !Context)
    return;
  for (const auto &[Store, Next] : DetachedRemovals)
    Store->insertBefore(Next->getIterator());
  DetachedRemovals.clear();
  for (const auto &[Id, Call] : Calls) {
    Plan.Branches.at(Id)->setSuccessor(0, OriginalTargets.at(Id));
    Call->getParent()->eraseFromParent();
  }
  Context->eraseFromParent();
  Chain->eraseFromParent();
  Destination->eraseFromParent();
  if (CreatedLongjmp)
    CreatedLongjmp->eraseFromParent();
  Context = nullptr;
  Calls.clear();
}

void SEHParentExitProjection::commit() {
  assert(!Committed && "parent continuation committed twice");
  Committed = true;
  for (const auto &[Store, Next] : DetachedRemovals)
    Store->deleteValue();
  DetachedRemovals.clear();
}

void SEHParentExitProjection::emitCapture(
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks) {
  assert(Committed && "capture follows successful callback outlining");
  if (!Context)
    return;
  auto *Original =
      llvm::cast<llvm::UncondBrInst>(Parent.getEntryBlock().getTerminator());
  auto *Normal = Original->getSuccessor(0);
  auto *Resume = llvm::BasicBlock::Create(
      Parent.getContext(), "registration.parent.resume", &Parent);
  auto *Invalid = llvm::BasicBlock::Create(
      Parent.getContext(), "registration.parent.invalid", &Parent);
  auto *Module = Parent.getParent();
  llvm::IRBuilder<> Builder(Original);
  auto *Head =
      Builder.CreateLoad(Builder.getInt32Ty(),
                         llvm::ConstantPointerNull::get(Builder.getPtrTy(257)));
  Head->setVolatile(true);
  Builder.CreateStore(Head, Chain)->setVolatile(true);
  auto *Frame = Builder.CreateCall(
      llvm::Intrinsic::getOrInsertDeclaration(
          Module, llvm::Intrinsic::frameaddress, {Builder.getPtrTy()}),
      {Builder.getInt32(0)});
  Builder.CreateStore(Frame, Context)->setVolatile(true);
  auto *SP = Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
      Module, llvm::Intrinsic::stacksave, {Builder.getPtrTy()}));
  auto *SPCell = Builder.CreateInBoundsGEP(Builder.getInt8Ty(), Context,
                                           Builder.getInt32(8));
  Builder.CreateStore(SP, SPCell)->setVolatile(true);
  auto *Capture =
      Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                             Module, llvm::Intrinsic::eh_sjlj_setjmp),
                         {Context});
  mark(*Capture, Owner, 0, 4);
  Builder.CreateCondBr(Builder.CreateICmpEQ(Capture, Builder.getInt32(0)),
                       Normal, Resume);
  Original->eraseFromParent();

  Builder.SetInsertPoint(Resume);
  auto *Selected = Builder.CreateLoad(Builder.getInt32Ty(), Destination);
  Selected->setVolatile(true);
  auto *Dispatch = Builder.CreateSwitch(Selected, Invalid, Plan.Sources.size());
  for (const auto &[Id, Exit] : Plan.Sources)
    Dispatch->addCase(Builder.getInt32(Id),
                      OriginalBlocks.at(Exit.TargetBlock));
  Builder.SetInsertPoint(Invalid);
  Builder.CreateCall(
      llvm::Intrinsic::getOrInsertDeclaration(Module, llvm::Intrinsic::trap));
  Builder.CreateUnreachable();
}

} // namespace neverd::x86_registration

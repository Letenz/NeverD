//===- COFFRegistrationIncomingFrame.cpp - PE32 caller frame proof --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameAddress.h"

#include "llvm/IR/Dominators.h"

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
namespace neverd::coff_registration {

llvm::Error
validateIncomingCallerFrame(const llvm::Function &Parent, const MedFunc &Source,
                            llvm::ArrayRef<const llvm::Function *> Functions,
                            std::set<const llvm::Instruction *> &Accesses,
                            std::set<const llvm::Instruction *> &Setup) {
  if (!Source.RegistrationStates ||
      !Source.RegistrationStates->IncomingFrameAccessesComplete)
    return rejectIR("incoming caller frame projection is not exact");
  using Key = std::pair<va_t, int>;
  std::map<Key, const RegistrationIncomingFrameAccess *> Expected;
  for (const auto &Block : Source.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)
        if (const auto *Access = Source.RegistrationStates->incomingFrameAccess(
                Op.Addr, Op.OriginSeq))
          if (!Expected.emplace(Key{Access->Address, Access->OpSeq}, Access)
                   .second)
            return rejectIR(
                "incoming caller frame has duplicate source identities");
  const llvm::AllocaInst *Slot = nullptr;
  const llvm::IntrinsicInst *Escape = nullptr;
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        if (const auto *MD = I.getMetadata(
                windows_eh_md::RegistrationCallerFrameAttachment)) {
          auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (Slot || Function != &Parent ||
              &Block != &Parent.getEntryBlock() || !Alloca ||
              !Alloca->isStaticAlloca() ||
              !Alloca->getAllocatedType()->isPointerTy() ||
              Alloca->getAllocatedType()->getPointerAddressSpace() ||
              Alloca->getAddressSpace() || MD->getNumOperands() != 1 ||
              metadataInteger(*MD, 0, 64) != Source.Entry)
            return rejectIR("incoming caller frame slot identity changed");
          Slot = Alloca;
        }
        if (const auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
            Function == &Parent && Call &&
            Call->getIntrinsicID() == llvm::Intrinsic::localescape)
          Escape = Call;
      }
  if (Expected.empty() != (Slot == nullptr))
    return rejectIR(
        "incoming caller frame is missing its checked recovery slot");
  auto IsSlot = [&](const llvm::Value *Value) {
    if (Value == Slot && Slot)
      return true;
    const auto *Recover = llvm::dyn_cast<llvm::IntrinsicInst>(Value);
    const auto Index = Recover && Recover->arg_size() == 3
                           ? integer(Recover->getArgOperand(2), 32)
                           : std::nullopt;
    return Slot && Recover &&
           Recover->getIntrinsicID() == llvm::Intrinsic::localrecover &&
           Recover->getArgOperand(0) == &Parent && Escape && Index &&
           *Index < Escape->arg_size() && Escape->getArgOperand(*Index) == Slot;
  };
  const llvm::StoreInst *Initializer = nullptr;
  if (Slot) {
    for (const auto *Function : Functions)
      for (const auto &Block : *Function)
        for (const auto &I : Block)
          if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
              Store && IsSlot(Store->getPointerOperand())) {
            const auto *Frame =
                llvm::dyn_cast<llvm::IntrinsicInst>(Store->getValueOperand());
            if (Initializer || Function != &Parent ||
                &Block != &Parent.getEntryBlock() ||
                Store->getPointerOperand() != Slot || Store->isAtomic() ||
                !Frame || Frame->getParent() != &Block ||
                Frame->getIntrinsicID() != llvm::Intrinsic::frameaddress ||
                Frame->arg_size() != 1 ||
                integer(Frame->getArgOperand(0), 32) != 0)
              return rejectIR(
                  "incoming caller frame lost its entry frame address");
            Initializer = Store;
            Setup.insert(Frame);
            Setup.insert(Store);
          }
    if (!Initializer)
      return rejectIR(
          "incoming caller frame is not initialized at machine entry");
    // No hidden alias may overwrite the runtime parent-frame pointer.
    for (const auto *Function : Functions)
      for (const auto &Block : *Function)
        for (const auto &I : Block)
          if (IsSlot(&I))
            for (const auto *User : I.users()) {
              if (User == Initializer || User == Escape)
                continue;
              const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
              if (!Load || Load->getPointerOperand() != &I ||
                  Load->isAtomic() || !Load->getType()->isPointerTy())
                return rejectIR(
                    "incoming caller frame slot escapes its recovery protocol");
            }
  }
  std::set<Key> Seen;
  llvm::DominatorTree Dominators(*const_cast<llvm::Function *>(&Parent));
  for (const auto *Function : Functions)
    for (const auto &Block : *Function)
      for (const auto &I : Block) {
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment);
        if (!MD)
          continue;
        const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
        const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
        auto Address = metadataInteger(*MD, 1, 64),
             Seq = metadataInteger(*MD, 2, 32);
        auto Offset = metadataInteger(*MD, 3, 32),
             Width = metadataInteger(*MD, 4, 16);
        auto Write = metadataInteger(*MD, 5, 1);
        if ((!Load && !Store) || MD->getNumOperands() != 6 ||
            metadataInteger(*MD, 0, 64) != Source.Entry || !Address || !Seq ||
            *Seq > INT_MAX || !Offset || *Offset < 4 || !Width || !*Width ||
            !Write)
          return rejectIR(
              "incoming caller frame access has a malformed identity");
        const Key Identity{*Address, int(*Seq)};
        const auto *Operation =
            I.getMetadata(windows_eh_md::RegistrationOperationAttachment);
        if (!Operation || Operation->getNumOperands() != 5 ||
            metadataInteger(*Operation, 0, 64) != Source.Entry ||
            metadataInteger(*Operation, 1, 64) != Address ||
            metadataInteger(*Operation, 2, 32) != Seq ||
            metadataInteger(*Operation, 4, 8) != uint8_t(bool(Store)))
          return rejectIR("incoming caller frame access changed its source "
                          "execution occurrence");
        auto ExpectedAccess = Expected.find(Identity);
        if (ExpectedAccess == Expected.end() || !Seen.insert(Identity).second ||
            ExpectedAccess->second->Offset != *Offset ||
            ExpectedAccess->second->Width != *Width ||
            ExpectedAccess->second->Write != bool(Store) ||
            *Write != bool(Store) ||
            (Load ? Load->isAtomic() || !Load->isVolatile() ||
                        Load->getAlign() != llvm::Align(1)
                  : Store->isAtomic() || !Store->isVolatile() ||
                        Store->getAlign() != llvm::Align(1)))
          return rejectIR("incoming caller frame access differs from checked "
                          "source memory");
        auto Size = Parent.getParent()->getDataLayout().getTypeStoreSize(
            Load ? Load->getType() : Store->getValueOperand()->getType());
        const auto *Pointer =
            Load ? Load->getPointerOperand() : Store->getPointerOperand();
        const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Pointer);
        const auto *Base =
            GEP ? llvm::dyn_cast<llvm::LoadInst>(GEP->getPointerOperand())
                : nullptr;
        auto Displacement = registration_frame::checkedByteGEPOffset(GEP);
        if (Size.isScalable() || Size.getFixedValue() != *Width || !GEP ||
            GEP->isInBounds() || GEP->hasNoUnsignedSignedWrap() ||
            !Displacement || *Displacement != int64_t(*Offset) || !Base ||
            !IsSlot(Base->getPointerOperand()) || Base->isAtomic() ||
            (Function == &Parent && !Dominators.dominates(Initializer, Base)))
          return rejectIR("incoming caller frame access changed its physical "
                          "stack projection");
        Accesses.insert(&I);
      }
  if (Seen.size() != Expected.size())
    return rejectIR("incoming caller frame access set is incomplete");
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
#endif

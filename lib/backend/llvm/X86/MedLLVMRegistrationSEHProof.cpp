//===- MedLLVMRegistrationSEHProof.cpp - PE32 SEH preflight ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHProof.h"

#include "neverd/Limits.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

namespace neverd::x86_registration {

std::optional<SEHFrameValues> getPrivateSEHFrameValues(
    const MedFunc &Func, const RegistrationStateAnalysis &States,
    llvm::function_ref<bool(const MedVar &, uint16_t)> IsPrivateStore) {
  // A synthetic source frame must stay private. Otherwise an unknown callee
  // can modify a source registration field without changing LLVM's live node.
  // Consume the shared LowIR frame domain, including spill/reload provenance,
  // then carry that fact through synthetic MedIR copies and PHIs.
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  auto Key = [](const MedVar &V) { return ValueKey{V.Kind, V.Id, V.SSAVer}; };
  std::set<std::pair<va_t, int>> FrameOccurrences;
  for (const auto &Value : States.FrameValues)
    FrameOccurrences.emplace(Value.Address, Value.OpSeq);
  SEHFrameValues Result;
  auto &FrameValues = Result.Values;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (!Op.Output.isConst() && Op.Output.Size &&
          (FrameOccurrences.count({Op.Addr, Op.OriginSeq}) ||
           Op.RegistrationRoot != MedOp::RegistrationRootKind::None))
        FrameValues.insert(Key(Op.Output));
  auto IsFrame = [&](const MedVar &V) {
    return !V.isConst() && FrameValues.count(Key(V));
  };
  bool Changed = true;
  size_t Work = 0;
  while (Changed) {
    Changed = false;
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        for (const auto &[Pred, Value] : Phi.Args)
          if (IsFrame(Value))
            Changed |= FrameValues.insert(Key(Phi.Output)).second;
      }
      for (const MedOp &Op : Block.Ops) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        if (!Op.Output.Size || Op.Output.isConst() || Op.Opcode == NdOp::LOAD ||
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          continue;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (IsFrame(Op.Inputs[I]))
            Changed |= FrameValues.insert(Key(Op.Output)).second;
      }
    }
  }
  for (const MedCallInfo &Call : Func.CallInfos)
    if (llvm::any_of(Call.Args, IsFrame))
      return std::nullopt;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      // Indexed source-memory receipts currently describe ordinary loads and
      // stores. Do not admit an unbound read-modify-write effect.
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG)
        return std::nullopt;
      if ((Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::COND_BR ||
           Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::CALL ||
           Op.Opcode == NdOp::INDIR_CALL) &&
          llvm::any_of(llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                       IsFrame))
        return std::nullopt;
      if ((Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          llvm::any_of(llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                       IsFrame))
        return std::nullopt;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          IsFrame(Op.Inputs[1]) &&
          Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS) {
        if (!IsPrivateStore(Op.Inputs[0], Op.Inputs[1].Size))
          return std::nullopt;
      }
    }

  return Result;
}

std::optional<std::set<llvm::Instruction *>> getCheckedSEHChainInstructions(
    llvm::Function &Parent, const RegistrationStateAnalysis &States,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationChainIR) {
  // Authenticate every emitted FS memory instruction, not just entries that
  // happen to survive in an occurrence map. Duplicate emission is unknown.
  std::set<llvm::Instruction *> ChainInstructions;
  for (const RegistrationChainAccess &Access : States.ChainAccesses) {
    auto It = RegistrationChainIR.find({Access.Address, Access.OpSeq});
    if (It == RegistrationChainIR.end() || !It->second ||
        !ChainInstructions.insert(It->second).second ||
        It->second->getFunction() != &Parent)
      return std::nullopt;
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(It->second);
    auto *Store = llvm::dyn_cast<llvm::StoreInst>(It->second);
    const bool Read =
        Access.AccessKind == RegistrationChainAccess::Kind::ReadPreviousHead ||
        Access.AccessKind == RegistrationChainAccess::Kind::ReadInstalledHead;
    llvm::Value *Pointer = Load    ? Load->getPointerOperand()
                           : Store ? Store->getPointerOperand()
                                   : nullptr;
    const auto *Offset =
        llvm::dyn_cast_or_null<llvm::ConstantPointerNull>(Pointer);
    if (Read != (Load != nullptr) || !Pointer || !Offset ||
        Pointer->getType()->getPointerAddressSpace() != 257 ||
        (Load && (!Load->getType()->isIntegerTy(32) || Load->isAtomic() ||
                  Load->isVolatile())) ||
        (Store && (!Store->getValueOperand()->getType()->isIntegerTy(32) ||
                   Store->isAtomic() || Store->isVolatile())))
      return std::nullopt;
  }
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block) {
      if (Instruction.isEHPad() || llvm::isa<llvm::InvokeInst>(Instruction) ||
          llvm::isa<llvm::CallBrInst>(Instruction) ||
          llvm::isa<llvm::IndirectBrInst>(Instruction) ||
          llvm::isa<llvm::ResumeInst>(Instruction))
        return std::nullopt;
      const llvm::Value *Pointer = nullptr;
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
        Pointer = Load->getPointerOperand();
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
        Pointer = Store->getPointerOperand();
      if (Pointer && Pointer->getType()->getPointerAddressSpace() == 257 &&
          !ChainInstructions.count(&Instruction))
        return std::nullopt;
      for (const llvm::Use &Operand : Instruction.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257 &&
            !ChainInstructions.count(&Instruction))
          return std::nullopt;
    }

  return ChainInstructions;
}

std::optional<SEHSourceOperations> getCheckedSEHSourceOperations(
    const MedFunc &Func,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationMemoryIR,
    const std::map<const llvm::CallInst *, va_t> &CallSiteAddrs) {
  const auto &States = *Func.RegistrationStates;

  std::vector<SourceOperation> SourceOperations;
  std::set<llvm::CallInst *> CookieCheckCalls;
  size_t MemoryOperations = 0;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      llvm::Instruction *Instruction = nullptr;
      uint8_t Kind;
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto It = RegistrationMemoryIR.find({Op.Addr, Op.OriginSeq});
        if (It == RegistrationMemoryIR.end() || !It->second)
          return std::nullopt;
        Instruction = It->second;
        Kind = Op.Opcode == NdOp::STORE;
        ++MemoryOperations;
      } else if (Op.Opcode == NdOp::CALL) {
        for (const auto &[Call, Address] : CallSiteAddrs)
          if (Address == Op.Addr) {
            if (Instruction)
              return std::nullopt;
            Instruction = const_cast<llvm::CallInst *>(Call);
          }
        if (!Instruction)
          return std::nullopt;
        Kind = States.cookieCheck(Op.Addr, Op.OriginSeq) ? 3 : 2;
        if (Kind == 3) {
          auto *Call = llvm::cast<llvm::CallInst>(Instruction);
          if (!States.SecurityCookiesComplete || !States.CookieCheckVA ||
              Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
              Op.Inputs[0].ConstVal != States.CookieCheckVA ||
              !Call->use_empty() || Call->isMustTailCall())
            return std::nullopt;
          CookieCheckCalls.insert(Call);
        }
      } else
        continue;
      if (Op.OriginSeq < 0 ||
          Instruction->getParent() != OriginalBlockMap.at(Block.Id))
        return std::nullopt;
      SourceOperations.push_back({Instruction, &Op, Block.Id, Kind});
    }
  if (MemoryOperations != RegistrationMemoryIR.size())
    return std::nullopt;
  if (CookieCheckCalls.size() != States.CookieChecks.size())
    return std::nullopt;

  return SEHSourceOperations{std::move(SourceOperations),
                             std::move(CookieCheckCalls)};
}

bool collectDeadFinallyResult(llvm::CallInst &Call,
                              std::vector<llvm::Instruction *> &Dead) {
  std::set<llvm::Instruction *> Seen;
  std::vector<llvm::Value *> Pending{&Call};
  size_t Work = 0;
  while (!Pending.empty()) {
    auto *Value = Pending.back();
    Pending.pop_back();
    for (auto *User : Value->users()) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (!Instruction || Instruction->getFunction() != Call.getFunction())
        return false;
      if (!Seen.insert(Instruction).second)
        continue;
      if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Instruction);
          Cast && Cast->getSrcTy()->isIntegerTy() &&
          Cast->getDestTy()->isIntegerTy()) {
        Dead.push_back(Cast);
        Pending.push_back(Cast);
        continue;
      }
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(Instruction);
      auto *Slot =
          Store ? llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand())
                : nullptr;
      if (!Store || Store->getValueOperand() != Value || Store->isVolatile() ||
          Store->isAtomic() || !Slot || !Slot->isStaticAlloca() ||
          !Slot->getAllocatedType()->isIntegerTy())
        return false;
      for (auto *SlotUser : Slot->users()) {
        auto *Definition = llvm::dyn_cast<llvm::StoreInst>(SlotUser);
        if (!Definition || Definition->getPointerOperand() != Slot ||
            Definition->isVolatile() || Definition->isAtomic())
          return false;
      }
      Dead.push_back(Store);
    }
  }
  return true;
}

} // namespace neverd::x86_registration

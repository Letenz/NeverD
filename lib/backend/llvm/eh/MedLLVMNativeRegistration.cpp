//===- MedLLVMNativeRegistration.cpp - Native PE32 SEH -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/Limits.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include <map>
#include <set>
#include <tuple>
#include <vector>

#define DEBUG_TYPE "neverd-x86-registration"

namespace neverd {
namespace {

constexpr auto Model = windows_eh_md::NativeProvenanceModel::X86RegistrationSEH;
using Role = windows_eh_md::NativeProvenanceRole;

bool mayUnwind(const llvm::CallInst &Call) {
  // A nounwind callee can still fault while this registration is active.
  return !Call.isMustTailCall() && !llvm::isa<llvm::IntrinsicInst>(Call);
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

} // namespace

bool MedLLVMEmitter::emitNativeX86RegistrationSEH(
    const MedFunc &Func, llvm::Function &Parent,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (TargetArch != Arch::X86 || TargetFormat != BinaryFormat::COFF ||
      !Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      Func.CalleePopBytes || !Func.RegistrationCallerCleanupABIComplete ||
      !FrameAlloca || FrameEntrySPOffset < 24 || Parent.hasPersonalityFn())
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const auto Source = classifyWindowsEHNativeSource(
      EH, TargetArch, TargetFormat, WindowsEHNativeCapability::IRLowering);
  if (!Source.canLowerNativeIR() ||
      Source.Model != WindowsEHNativeSourceModel::X86RegistrationSEH)
    return false;
  const RegistrationChainInfo &Chain = *EH.Registration;
  const RegistrationStateAnalysis &States = *Func.RegistrationStates;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.IncomingFrameAccessesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      States.ChainAccesses.empty() ||
      States.ChainAccesses.size() != RegistrationChainIR.size() ||
      !med_llvm_eh::collectExactSourceCallAddresses(Parent, CallSiteAddrs))
    return false;
  const auto AsyncFlag = med_llvm_eh::classifyI32ModuleFlag(
      *Mod, "eh-asynch", llvm::Module::Warning, 1);
  if (AsyncFlag == med_llvm_eh::I32ModuleFlagState::Conflict)
    return false;
  const auto FrameBytes = FrameAlloca->getAllocationSize(Mod->getDataLayout());
  if (!FrameBytes || FrameBytes->isScalable() ||
      FrameEntrySPOffset > FrameBytes->getFixedValue())
    return false;

  // A synthetic source frame must stay private. Otherwise an unknown callee
  // can modify a source registration field without changing LLVM's live node.
  // Consume the shared LowIR frame domain, including spill/reload provenance,
  // then carry that fact through synthetic MedIR copies and PHIs.
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  auto Key = [](const MedVar &V) { return ValueKey{V.Kind, V.Id, V.SSAVer}; };
  std::set<std::pair<va_t, int>> FrameOccurrences;
  for (const auto &Value : States.FrameValues)
    FrameOccurrences.emplace(Value.Address, Value.OpSeq);
  std::set<ValueKey> FrameValues;
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
          return false;
        for (const auto &[Pred, Value] : Phi.Args)
          if (IsFrame(Value))
            Changed |= FrameValues.insert(Key(Phi.Output)).second;
      }
      for (const MedOp &Op : Block.Ops) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
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
      return false;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      // Indexed source-memory receipts currently describe ordinary loads and
      // stores. Do not admit an unbound read-modify-write effect.
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG)
        return false;
      if ((Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::COND_BR ||
           Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::CALL ||
           Op.Opcode == NdOp::INDIR_CALL) &&
          llvm::any_of(llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                       IsFrame))
        return false;
      if ((Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          llvm::any_of(llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                       IsFrame))
        return false;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          IsFrame(Op.Inputs[1]) &&
          Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS) {
        const auto Slot = canonicalFrameSlotKey(Op.Inputs[0], true);
        if (!Slot || Slot->second < -int64_t(FrameEntrySPOffset) ||
            Op.Inputs[1].Size > FrameBytes->getFixedValue() ||
            Slot->second >
                int64_t(FrameBytes->getFixedValue() - Op.Inputs[1].Size) -
                    int64_t(FrameEntrySPOffset))
          return false;
      }
    }

  std::map<va_t, const MedBlock *> BlocksAt;
  std::map<int, const RegistrationBlockState *> BlockStates;
  for (const RegistrationBlockState &State : States.Blocks)
    if (!BlockStates.emplace(State.BlockId, &State).second)
      return false;
  std::map<llvm::BasicBlock *, int32_t> Active;
  std::set<llvm::BasicBlock *> CallbackBlocks;
  const int32_t Sentinel = *Chain.SeededTryLevel;
  for (const MedBlock &Block : Func.Blocks) {
    auto IR = OriginalBlockMap.find(Block.Id);
    auto State = BlockStates.find(Block.Id);
    if (!BlocksAt.emplace(Block.StartAddr, &Block).second ||
        IR == OriginalBlockMap.end() || !IR->second ||
        IR->second->getParent() != &Parent || !IR->second->getTerminator() ||
        State == BlockStates.end() ||
        State->second->Range.Begin != Block.StartAddr ||
        State->second->Range.End != Block.EndAddr || State->second->Unknown)
      return false;
    if (State->second->CallbackOnly) {
      CallbackBlocks.insert(IR->second);
      continue;
    }
    if (State->second->Levels.size() > 1)
      return false;
    const int32_t Level = State->second->Levels.empty()
                              ? Sentinel
                              : State->second->Levels.front();
    if (Level != Sentinel &&
        (Level < 0 || uint32_t(Level) >= Chain.Scopes.size()))
      return false;
    Active.emplace(IR->second, Level);
  }
  if (BlockStates.size() != Func.Blocks.size())
    return false;
  auto BlockAt = [&](va_t VA) -> llvm::BasicBlock * {
    auto It = BlocksAt.find(VA);
    return It == BlocksAt.end() ? nullptr : OriginalBlockMap.at(It->second->Id);
  };

  // Authenticate every emitted FS memory instruction, not just entries that
  // happen to survive in an occurrence map. Duplicate emission is unknown.
  std::set<llvm::Instruction *> ChainInstructions;
  for (const RegistrationChainAccess &Access : States.ChainAccesses) {
    auto It = RegistrationChainIR.find({Access.Address, Access.OpSeq});
    if (It == RegistrationChainIR.end() || !It->second ||
        !ChainInstructions.insert(It->second).second ||
        It->second->getFunction() != &Parent)
      return false;
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
      return false;
  }
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block) {
      if (Instruction.isEHPad() || llvm::isa<llvm::InvokeInst>(Instruction) ||
          llvm::isa<llvm::CallBrInst>(Instruction) ||
          llvm::isa<llvm::IndirectBrInst>(Instruction) ||
          llvm::isa<llvm::ResumeInst>(Instruction))
        return false;
      const llvm::Value *Pointer = nullptr;
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
        Pointer = Load->getPointerOperand();
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
        Pointer = Store->getPointerOperand();
      if (Pointer && Pointer->getType()->getPointerAddressSpace() == 257 &&
          !ChainInstructions.count(&Instruction))
        return false;
      for (const llvm::Use &Operand : Instruction.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257 &&
            !ChainInstructions.count(&Instruction))
          return false;
    }

  struct Region {
    llvm::BasicBlock *Handler = nullptr;
    size_t Callback = 0;
    llvm::BasicBlock *Unwind = nullptr;
    llvm::mc_rewrite::RewriteWinEHSemanticToken Token;
  };
  std::vector<Region> Regions;
  std::vector<X86RegistrationCallbackRequest> Requests;
  std::vector<std::vector<X86RegistrationRootSeed>> Seeds;
  std::map<std::pair<va_t, bool>, size_t> CallbackIndices;
  Regions.reserve(Chain.Scopes.size());
  Requests.reserve(Chain.Scopes.size());
  Seeds.reserve(Chain.Scopes.size());
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (size_t Index = 0; Index < Chain.Scopes.size(); ++Index) {
    const RegistrationScopeRecord &Scope = Chain.Scopes[Index];
    auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
        EH, Arch::X86, static_cast<uint32_t>(Index));
    if (!Token)
      return false;
    Region R;
    R.Token = *Token;
    R.Handler = Scope.IsFinally ? nullptr : BlockAt(Scope.HandlerVA);
    if (!Scope.IsFinally && (!R.Handler || CallbackBlocks.count(R.Handler) ||
                             !Active.count(R.Handler) ||
                             Active.at(R.Handler) != Scope.EnclosingLevel))
      return false;
    const va_t CallbackVA = Scope.IsFinally ? Scope.HandlerVA : Scope.FilterVA;
    auto [Callback, New] = CallbackIndices.emplace(
        std::make_pair(CallbackVA, Scope.IsFinally), Requests.size());
    R.Callback = Callback->second;
    if (New) {
      X86RegistrationCallbackRequest Request;
      Request.Entry = BlockAt(CallbackVA);
      Request.Kind = Scope.IsFinally ? X86RegistrationCallbackKind::Finally
                                     : X86RegistrationCallbackKind::Filter;
      Request.Name = Parent.getName().str() + ".registration.callback." +
                     std::to_string(Requests.size());
      if (!Request.Entry || !CallbackBlocks.count(Request.Entry) ||
          Mod->getNamedValue(Request.Name))
        return false;
      std::set<llvm::BasicBlock *> Reached;
      std::vector<llvm::BasicBlock *> Pending{Request.Entry};
      while (!Pending.empty()) {
        llvm::BasicBlock *Block = Pending.back();
        Pending.pop_back();
        if (!Reached.insert(Block).second)
          continue;
        if (!Block->getTerminator() ||
            Reached.size() > limits::kMaxRegistrationEHRecords)
          return false;
        Request.Blocks.push_back(Block);
        const MedBlock *SourceBlock = nullptr;
        for (const auto &[VA, Candidate] : BlocksAt)
          if (OriginalBlockMap.at(Candidate->Id) == Block)
            SourceBlock = Candidate;
        if (!SourceBlock)
          return false;
        for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores)
          if (Store.StoreVA >= SourceBlock->StartAddr &&
              Store.StoreVA < SourceBlock->EndAddr)
            return false;
        for (llvm::BasicBlock *Successor : llvm::successors(Block))
          Pending.push_back(Successor);
      }
      Seeds.emplace_back();
      for (const MedOp &Op : BlocksAt.at(CallbackVA)->Ops) {
        if (Op.RegistrationRoot == MedOp::RegistrationRootKind::None)
          continue;
        if (Op.Opcode != NdOp::COPY || Op.Addr != CallbackVA ||
            Op.Output.Kind != MedVar::Reg || Op.Output.Size != 4 ||
            Op.Output.SSAVer <= 0)
          return false;
        const bool FP = Op.RegistrationRoot ==
                        MedOp::RegistrationRootKind::EstablishedFramePointer;
        if (Op.Output.RegOff != (FP ? TRI.FramePointer : TRI.StackPointer))
          return false;
        auto Slot = VarAllocs.find({Op.Output.Id, Op.Output.SSAVer});
        if (Slot == VarAllocs.end())
          return false;
        llvm::StoreInst *Definition = nullptr;
        for (llvm::User *User : Slot->second->users())
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
              Store && Store->getPointerOperand() == Slot->second &&
              Store->getParent() == Request.Entry) {
            if (Definition)
              return false;
            Definition = Store;
          }
        if (!Definition)
          return false;
        Seeds.back().push_back(
            {Definition, FP ? X86RegistrationRootKind::FramePointer
                            : X86RegistrationRootKind::StackPointer});
        if (FP)
          Request.Frame.EstablishedFramePointerOffset = FrameEntrySPOffset - 4;
        else
          Request.Frame.InferStackBounds = true;
      }
      Request.Frame.RootSeeds = Seeds.back();
      if (!Scope.IsFinally)
        Request.Frame.ExceptionPointersOffset = FrameEntrySPOffset - 24;
      if (Request.Frame.EstablishedFramePointerOffset || !Scope.IsFinally)
        Request.Frame.SyntheticFrame = FrameAlloca;
      Requests.push_back(std::move(Request));
    }
    Regions.push_back(R);
  }

  using IncomingKey = std::pair<va_t, int>;
  std::map<IncomingKey, const RegistrationIncomingFrameAccess *> Incoming;
  for (const auto &Block : Func.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)
        if (const auto *Found =
                States.incomingFrameAccess(Op.Addr, Op.OriginSeq)) {
          const auto &Access = *Found;
          auto I = RegistrationIncomingIR.find({Access.Address, Access.OpSeq});
          auto *Load = I != RegistrationIncomingIR.end()
                           ? llvm::dyn_cast_or_null<llvm::LoadInst>(I->second)
                           : nullptr;
          auto *Store = I != RegistrationIncomingIR.end()
                            ? llvm::dyn_cast_or_null<llvm::StoreInst>(I->second)
                            : nullptr;
          if (!Incoming
                   .emplace(IncomingKey{Access.Address, Access.OpSeq}, &Access)
                   .second ||
              Access.Offset < 4 || Access.Width == 0 ||
              int64_t(Access.Offset) + Access.Width > INT32_MAX ||
              (Access.Write ? !Store || Store->isAtomic()
                            : !Load || Load->isAtomic()) ||
              (Access.Write && IsFrame(Op.Inputs[1])))
            return false;
          auto *Type = Access.Write ? Store->getValueOperand()->getType()
                                    : Load->getType();
          auto Size = Mod->getDataLayout().getTypeStoreSize(Type);
          if (Size.isScalable() || Size.getFixedValue() != Access.Width)
            return false;
        }
  if (Incoming.size() != RegistrationIncomingIR.size())
    return false;

  struct SourceOperation {
    llvm::Instruction *Instruction;
    const MedOp *Operation;
    int Block;
    uint8_t Kind;
  };
  std::vector<SourceOperation> SourceOperations;
  size_t MemoryOperations = 0;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      llvm::Instruction *Instruction = nullptr;
      uint8_t Kind;
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto It = RegistrationMemoryIR.find({Op.Addr, Op.OriginSeq});
        if (It == RegistrationMemoryIR.end() || !It->second)
          return false;
        Instruction = It->second;
        Kind = Op.Opcode == NdOp::STORE;
        ++MemoryOperations;
      } else if (Op.Opcode == NdOp::CALL) {
        for (const auto &[Call, Address] : CallSiteAddrs)
          if (Address == Op.Addr) {
            if (Instruction)
              return false;
            Instruction = const_cast<llvm::CallInst *>(Call);
          }
        if (!Instruction)
          return false;
        Kind = 2;
      } else
        continue;
      if (Op.OriginSeq < 0 ||
          Instruction->getParent() != OriginalBlockMap.at(Block.Id))
        return false;
      SourceOperations.push_back({Instruction, &Op, Block.Id, Kind});
    }
  if (MemoryOperations != RegistrationMemoryIR.size())
    return false;

  // Each scope must actually participate in the proven dispatch graph.
  std::vector<bool> Used(Regions.size());
  for (const auto &[Block, Level] : Active)
    for (int32_t State = Level; State >= 0;
         State = Chain.Scopes[State].EnclosingLevel)
      Used[State] = true;
  if (llvm::any_of(Used, [](bool Used) { return !Used; }))
    return false;

  struct CallPlan {
    llvm::CallInst *Call;
    va_t Address;
    int32_t State;
    std::optional<size_t> Finally;
  };
  std::map<va_t, va_t> DirectCalls;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
          !DirectCalls.emplace(Op.Addr, Op.Inputs[0].ConstVal).second)
        return false;
  std::set<llvm::BasicBlock *> AllCallbackBlocks;
  for (const auto &Request : Requests)
    for (llvm::BasicBlock *Block : Request.Blocks)
      if (!AllCallbackBlocks.insert(Block).second)
        return false;
  // Callback frame recovery changes EBP while the compiler owns FS:[0]. A
  // source callback which observes that chain needs a separate remap proof.
  for (llvm::Instruction *Instruction : ChainInstructions)
    if (AllCallbackBlocks.count(Instruction->getParent()))
      return false;
  std::vector<CallPlan> Calls;
  std::vector<llvm::Instruction *> DeadFinallyResults;
  for (const auto &[Call, Address] : CallSiteAddrs) {
    auto *CallBlock = const_cast<llvm::BasicBlock *>(Call->getParent());
    if (AllCallbackBlocks.count(CallBlock))
      continue;
    auto State = Active.find(CallBlock);
    if (State == Active.end() || !Call->getNextNode() || Call->isMustTailCall())
      return false;
    std::optional<size_t> Finally;
    if (auto Direct = DirectCalls.find(Address); Direct != DirectCalls.end())
      if (auto Callback = CallbackIndices.find({Direct->second, true});
          Callback != CallbackIndices.end())
        Finally = Callback->second;
    if (Finally && !collectDeadFinallyResult(
                       *const_cast<llvm::CallInst *>(Call), DeadFinallyResults))
      return false;
    if ((State->second >= 0 && mayUnwind(*Call)) || Finally)
      Calls.push_back({const_cast<llvm::CallInst *>(Call), Address,
                       State->second, Finally});
  }
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call) &&
          !CallSiteAddrs.count(Call))
        return false;

  auto *PersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), {}, true);
  const llvm::StringRef PersonalityName =
      EH.Personality == ExceptionPersonality::ExceptHandler4
          ? "_except_handler4"
          : "_except_handler3";
  if (!med_llvm_eh::canMaterializeExternalFunctionDeclaration(
          *Mod, PersonalityName, PersonalityTy))
    return false;
  llvm::Function *Previous = Mod->getFunction(PersonalityName);
  auto *Personality = llvm::cast<llvm::Function>(
      Mod->getOrInsertFunction(PersonalityName, PersonalityTy).getCallee());
  Parent.setPersonalityFn(Personality);
  struct SourceOriginal {
    llvm::Instruction *Instruction;
    llvm::MDNode *Marker;
    bool Volatile;
  };
  std::vector<SourceOriginal> SourceOriginals;
  for (const auto &Event : SourceOperations) {
    auto *I = Event.Instruction;
    const auto *Load = llvm::dyn_cast<llvm::LoadInst>(I);
    const auto *Store = llvm::dyn_cast<llvm::StoreInst>(I);
    SourceOriginals.push_back(
        {I, I->getMetadata(windows_eh_md::RegistrationOperationAttachment),
         Load ? Load->isVolatile() : Store && Store->isVolatile()});
    I->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        llvm::MDNode::get(
            *Ctx, {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                   med_llvm_eh::mdUInt(*Ctx, Event.Operation->Addr, 64),
                   med_llvm_eh::mdUInt(*Ctx, Event.Operation->OriginSeq, 32),
                   med_llvm_eh::mdUInt(*Ctx, Event.Block, 32),
                   med_llvm_eh::mdUInt(*Ctx, Event.Kind, 8)}));
  }
  llvm::Function *PreviousSideEffect = Mod->getFunction("llvm.sideeffect");
  std::vector<llvm::Instruction *> SourceAnchors;
  auto *SideEffect =
      llvm::Intrinsic::getOrInsertDeclaration(Mod, llvm::Intrinsic::sideeffect);
  for (const MedBlock &Block : Func.Blocks) {
    auto *IR = OriginalBlockMap.at(Block.Id);
    for (bool Enter : {true, false}) {
      llvm::IRBuilder<> B(Enter ? &*IR->getFirstInsertionPt()
                                : IR->getTerminator());
      auto *Anchor = B.CreateCall(SideEffect);
      Anchor->setMetadata(
          windows_eh_md::RegistrationBlockAttachment,
          llvm::MDNode::get(*Ctx,
                            {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                             med_llvm_eh::mdUInt(*Ctx, Block.StartAddr, 64),
                             med_llvm_eh::mdUInt(*Ctx, Block.EndAddr, 64),
                             med_llvm_eh::mdUInt(*Ctx, Block.Id, 32),
                             med_llvm_eh::mdUInt(*Ctx, Enter, 1)}));
      SourceAnchors.push_back(Anchor);
    }
  }
  std::vector<llvm::Instruction *> IncomingSetup;
  struct IncomingOriginal {
    llvm::Value *Pointer;
    llvm::MDNode *Marker;
    bool Volatile;
  };
  std::map<llvm::Instruction *, IncomingOriginal> OriginalIncomingPointers;
  llvm::Function *PreviousFrameAddress =
      Mod->getFunction("llvm.frameaddress.p0");
  if (!Incoming.empty()) {
    llvm::IRBuilder<> Entry(Parent.getEntryBlock().getTerminator());
    auto *Slot = Entry.CreateAlloca(Entry.getPtrTy(), nullptr,
                                    "registration.caller.frame");
    IncomingSetup.push_back(Slot);
    Slot->setMetadata(
        windows_eh_md::RegistrationCallerFrameAttachment,
        llvm::MDNode::get(*Ctx,
                          {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64)}));
    auto *Frame = Entry.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(
            Mod, llvm::Intrinsic::frameaddress, {Entry.getPtrTy()}),
        {Entry.getInt32(0)});
    IncomingSetup.push_back(Frame);
    IncomingSetup.push_back(Entry.CreateStore(Frame, Slot));
    for (const auto &[Key, Access] : Incoming) {
      auto *Instruction = RegistrationIncomingIR.at(Key);
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(Instruction);
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(Instruction);
      llvm::IRBuilder<> B(Instruction);
      auto *Base = B.CreateLoad(B.getPtrTy(), Slot, "registration.caller.base");
      IncomingSetup.push_back(Base);
      auto *Pointer = llvm::cast<llvm::Instruction>(
          B.CreateGEP(B.getInt8Ty(), Base, B.getInt32(Access->Offset),
                      "registration.incoming"));
      IncomingSetup.push_back(Pointer);
      OriginalIncomingPointers.emplace(
          Instruction,
          IncomingOriginal{
              Load ? Load->getPointerOperand() : Store->getPointerOperand(),
              Instruction->getMetadata(
                  windows_eh_md::RegistrationIncomingFrameAttachment),
              Load ? Load->isVolatile() : Store->isVolatile()});
      if (Load)
        Load->setVolatile(true);
      else
        Store->setVolatile(true);
      Instruction->setOperand(Load ? 0 : 1, Pointer);
      Instruction->setMetadata(
          windows_eh_md::RegistrationIncomingFrameAttachment,
          llvm::MDNode::get(*Ctx,
                            {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                             med_llvm_eh::mdUInt(*Ctx, Access->Address, 64),
                             med_llvm_eh::mdUInt(*Ctx, Access->OpSeq, 32),
                             med_llvm_eh::mdUInt(*Ctx, Access->Offset, 32),
                             med_llvm_eh::mdUInt(*Ctx, Access->Width, 16),
                             med_llvm_eh::mdUInt(*Ctx, Access->Write, 1)}));
    }
  }
  auto Outlined = outlineX86RegistrationCallbacks(Parent, Requests);
  if (!Outlined) {
    const std::string Detail = llvm::toString(Outlined.takeError());
    LLVM_DEBUG(llvm::dbgs() << Detail << '\n');
    for (const auto &[Instruction, Original] : OriginalIncomingPointers) {
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Instruction))
        Load->setVolatile(Original.Volatile);
      else
        llvm::cast<llvm::StoreInst>(Instruction)
            ->setVolatile(Original.Volatile);
      Instruction->setOperand(llvm::isa<llvm::LoadInst>(Instruction) ? 0 : 1,
                              Original.Pointer);
      Instruction->setMetadata(
          windows_eh_md::RegistrationIncomingFrameAttachment, Original.Marker);
    }
    for (auto *Instruction : llvm::reverse(IncomingSetup))
      Instruction->eraseFromParent();
    for (const auto &Original : SourceOriginals) {
      Original.Instruction->setMetadata(
          windows_eh_md::RegistrationOperationAttachment, Original.Marker);
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Original.Instruction))
        Load->setVolatile(Original.Volatile);
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Original.Instruction))
        Store->setVolatile(Original.Volatile);
    }
    for (auto *Anchor : SourceAnchors)
      Anchor->eraseFromParent();
    if (!PreviousSideEffect && SideEffect->use_empty())
      SideEffect->eraseFromParent();
    if (!PreviousFrameAddress)
      if (auto *FrameAddress = Mod->getFunction("llvm.frameaddress.p0");
          FrameAddress && FrameAddress->use_empty())
        FrameAddress->eraseFromParent();
    Parent.setPersonalityFn(nullptr);
    if (!Previous)
      Personality->eraseFromParent();
    return false;
  }

  // Commit. All source occurrences, callbacks and control edges are closed.
  RegistrationIncomingIR.clear();
  RegistrationMemoryIR.clear();
  std::vector<llvm::Function *> SourceFunctions{&Parent};
  SourceFunctions.insert(SourceFunctions.end(), Outlined->begin(),
                         Outlined->end());
  for (auto *Function : SourceFunctions)
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (I.getMetadata(windows_eh_md::RegistrationOperationAttachment)) {
          if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
            Load->setVolatile(true);
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
            Store->setVolatile(true);
        }
  FrameAlloca->setMetadata(
      windows_eh_md::RegistrationFrameAttachment,
      llvm::MDNode::get(*Ctx,
                        {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                         med_llvm_eh::mdUInt(*Ctx, FrameEntrySPOffset, 64)}));
  auto *None = llvm::ConstantTokenNone::get(*Ctx);
  auto *I8 = llvm::Type::getInt8Ty(*Ctx);
  auto *I32 = llvm::Type::getInt32Ty(*Ctx);
  auto SetSourceRuntimeState = [&](llvm::IRBuilder<> &B, int32_t State) {
    auto *Slot = B.CreateInBoundsGEP(I8, FrameAlloca,
                                     B.getInt32(FrameEntrySPOffset - 8));
    auto *Store = B.CreateStore(B.getInt32(State), Slot);
    Store->setAlignment(llvm::Align(1));
    Store->setVolatile(true);
  };
  for (size_t I = 0; I < Regions.size(); ++I) {
    const auto &Scope = Chain.Scopes[I];
    Region &R = Regions[I];
    llvm::BasicBlock *Outer = Scope.EnclosingLevel >= 0
                                  ? Regions[Scope.EnclosingLevel].Unwind
                                  : nullptr;
    llvm::Function *Callback = (*Outlined)[R.Callback];
    auto *Dispatch = llvm::BasicBlock::Create(
        *Ctx, "registration.dispatch." + std::to_string(I), &Parent);
    llvm::IRBuilder<> B(Dispatch);
    if (Scope.IsFinally) {
      auto *Pad = B.CreateCleanupPad(None);
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.Token))
        llvm_unreachable("prevalidated registration token rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, Model, Role::RegionDispatch, EH.CodeRange.Begin, Scope.HandlerVA,
          I, 0, Pad, Scope.HandlerVA, 1);
      SetSourceRuntimeState(B, Scope.EnclosingLevel);
      auto *Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress));
      llvm::SmallVector<llvm::Value *, 1> BundleInputs{Pad};
      llvm::OperandBundleDef Bundle("funclet", BundleInputs);
      B.CreateCall(Callback, {B.getInt8(1), Frame}, {Bundle});
      B.CreateCleanupRet(Pad, Outer);
    } else {
      auto *Switch = B.CreateCatchSwitch(None, Outer, 1);
      auto *PadBB = llvm::BasicBlock::Create(
          *Ctx, "registration.catch." + std::to_string(I), &Parent);
      Switch->addHandler(PadBB);
      llvm::IRBuilder<> PB(PadBB);
      auto *Pad = PB.CreateCatchPad(Switch, {Callback});
      if (!med_llvm_eh::attachRewriteWinEHSemanticToken(*Pad, R.Token))
        llvm_unreachable("prevalidated registration token rejected");
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          PB, Model, Role::RegionDispatch, EH.CodeRange.Begin, Scope.HandlerVA,
          I, 0, Pad, Scope.FilterVA, 0);
      SetSourceRuntimeState(PB, Scope.EnclosingLevel);
      PB.CreateCatchRet(Pad, R.Handler);
      llvm::IRBuilder<> HB(&*R.Handler->getFirstInsertionPt());
      med_llvm_eh::emitWindowsEHProvenanceAnchor(HB, Model, Role::HandlerTarget,
                                                 EH.CodeRange.Begin,
                                                 Scope.HandlerVA, I, 0);
    }
    R.Unwind = Dispatch;
  }
  for (const auto &[Identity, Index] : CallbackIndices) {
    llvm::Function *Callback = (*Outlined)[Index];
    llvm::IRBuilder<> B(Callback->getEntryBlock().getTerminator());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        B, Model, Role::RegistrationCallback, EH.CodeRange.Begin,
        Identity.first, Index, 0, nullptr, 0, Identity.second);
  }

  for (const RegistrationChainAccess &Access : States.ChainAccesses) {
    llvm::Instruction *Instruction =
        RegistrationChainIR.at({Access.Address, Access.OpSeq});
    llvm::IRBuilder<> B(Instruction);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        B, Model, Role::RegistrationChainAccess, EH.CodeRange.Begin,
        Access.Address, 0, Access.OpSeq, nullptr, Access.EndAddress,
        static_cast<uint32_t>(Access.AccessKind));
    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Instruction)) {
      llvm::Value *Value = nullptr;
      if (Access.AccessKind ==
          RegistrationChainAccess::Kind::ReadPreviousHead) {
        auto *Head = B.CreateLoad(I32, Load->getPointerOperand());
        Value = B.CreateLoad(I32, B.CreateIntToPtr(Head, B.getPtrTy()));
        llvm::cast<llvm::LoadInst>(Value)->setAlignment(llvm::Align(1));
      } else {
        auto *Record = B.CreateInBoundsGEP(I8, FrameAlloca,
                                           B.getInt32(FrameEntrySPOffset - 20));
        Value = B.CreatePtrToInt(Record, I32);
      }
      Load->replaceAllUsesWith(Value);
    }
    Instruction->eraseFromParent();
  }
  RegistrationChainIR.clear();

  for (auto *Instruction : llvm::reverse(DeadFinallyResults))
    Instruction->eraseFromParent();

  uint64_t ProtectedCalls = 0;
  for (const CallPlan &Plan : Calls) {
    llvm::CallInst *Call = Plan.Call;
    if (Plan.Finally) {
      llvm::IRBuilder<> B(Call);
      auto *Frame = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
          Mod, llvm::Intrinsic::localaddress));
      auto *Replacement =
          B.CreateCall((*Outlined)[*Plan.Finally], {B.getInt8(0), Frame});
      Replacement->copyMetadata(*Call);
      va_t FinallyVA = 0;
      for (const auto &[Identity, Index] : CallbackIndices)
        if (Identity.second && Index == *Plan.Finally)
          FinallyVA = Identity.first;
      Replacement->setMetadata(
          windows_eh_md::RegistrationFinallyCallAttachment,
          llvm::MDNode::get(*Ctx,
                            {med_llvm_eh::mdUInt(*Ctx, EH.CodeRange.Begin, 64),
                             med_llvm_eh::mdUInt(*Ctx, Plan.Address, 64),
                             med_llvm_eh::mdUInt(*Ctx, FinallyVA, 64)}));
      CallSiteAddrs.erase(Call);
      Call->eraseFromParent();
      Call = Replacement;
      CallSiteAddrs.emplace(Call, Plan.Address);
    }
    if (Plan.State < 0)
      continue;
    ++ProtectedCalls;
    llvm::BasicBlock *Block = Call->getParent();
    auto *Continue =
        Block->splitBasicBlock(Call->getNextNode(), "registration.call.cont");
    auto *Branch = Block->getTerminator();
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (llvm::Value *Arg : Call->args())
      Args.push_back(Arg);
    auto *Invoke = llvm::InvokeInst::Create(
        Call->getFunctionType(), Call->getCalledOperand(), Continue,
        Regions[Plan.State].Unwind, Args, Call->getName(),
        Branch->getIterator());
    Invoke->setCallingConv(Call->getCallingConv());
    Invoke->setAttributes(Call->getAttributes());
    Invoke->copyMetadata(*Call);
    llvm::IRBuilder<> B(Invoke);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(B, Model, Role::ProtectedInvoke,
                                               EH.CodeRange.Begin, Plan.Address,
                                               Plan.State, 0);
    Call->replaceAllUsesWith(Invoke);
    CallSiteAddrs.erase(Call);
    Call->eraseFromParent();
    Branch->eraseFromParent();
  }

  llvm::Function *TryBegin = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_scope_begin);
  llvm::Function *TryEnd = llvm::Intrinsic::getOrInsertDeclaration(
      Mod, llvm::Intrinsic::seh_scope_end);
  for (const MedBlock &SourceBlock : Func.Blocks) {
    llvm::BasicBlock *Block = OriginalBlockMap.at(SourceBlock.Id);
    auto State = Active.find(Block);
    if (State == Active.end())
      continue;
    // Preserve asynchronous fault ordering across compiler-owned state stores.
    // The source state is constant within each checked Med block. Every next
    // block explicitly establishes its own state, including nested exits.
    auto Volatilize = [](llvm::BasicBlock &Part) {
      for (llvm::Instruction &Instruction : Part) {
        if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
          Load->setVolatile(true);
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
          Store->setVolatile(true);
      }
    };
    Volatilize(*Block);
    if (State->second < 0) {
      llvm::IRBuilder<> B(&*Block->getFirstInsertionPt());
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          B, Model, Role::RangeEnterTarget, EH.CodeRange.Begin,
          SourceBlock.StartAddr, SourceBlock.Id, 0, nullptr,
          SourceBlock.EndAddr, static_cast<uint32_t>(State->second));
      B.CreateCall(TryEnd);
      llvm::IRBuilder<> Exit(Block->getTerminator());
      med_llvm_eh::emitWindowsEHProvenanceAnchor(
          Exit, Model, Role::RangeExitTarget, EH.CodeRange.Begin,
          SourceBlock.EndAddr, SourceBlock.Id, 0, nullptr,
          SourceBlock.StartAddr, static_cast<uint32_t>(State->second));
      continue;
    }
    // The final original terminator may now live in a call continuation.
    // Follow the newly created normal edges to the source block's exit.
    llvm::BasicBlock *Exit = Block;
    while (auto *Invoke =
               llvm::dyn_cast<llvm::InvokeInst>(Exit->getTerminator())) {
      Exit = Invoke->getNormalDest();
      Volatilize(*Exit);
    }
    auto *Term = Exit->getTerminator();
    auto *After = Exit->splitBasicBlock(Term, "registration.range.exit");
    Exit->getTerminator()->eraseFromParent();
    llvm::IRBuilder<> EB(Exit);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        EB, Model, Role::RangeExit, EH.CodeRange.Begin, SourceBlock.EndAddr,
        State->second, SourceBlock.Id);
    EB.CreateInvoke(TryEnd, After, Regions[State->second].Unwind);
    llvm::IRBuilder<> AB(After->getTerminator());
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        AB, Model, Role::RangeExitTarget, EH.CodeRange.Begin,
        SourceBlock.EndAddr, SourceBlock.Id, 0, nullptr, SourceBlock.StartAddr,
        static_cast<uint32_t>(State->second));
    auto *Begin = llvm::BasicBlock::Create(*Ctx, "registration.range.enter",
                                           &Parent, Block);
    Block->replaceAllUsesWith(Begin);
    llvm::IRBuilder<> BB(Begin);
    med_llvm_eh::emitWindowsEHProvenanceAnchor(
        BB, Model, Role::RangeEnter, EH.CodeRange.Begin, SourceBlock.StartAddr,
        State->second, SourceBlock.Id);
    BB.CreateInvoke(TryBegin, Block, Regions[State->second].Unwind);
  }
  for (llvm::BasicBlock *Block : AllCallbackBlocks) {
    for (llvm::Instruction &Instruction : *Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        CallSiteAddrs.erase(Call);
    Block->dropAllReferences();
  }
  for (llvm::BasicBlock *Block : AllCallbackBlocks)
    Block->eraseFromParent();
  if (AsyncFlag == med_llvm_eh::I32ModuleFlagState::Absent)
    Mod->addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  Parent.addFnAttr(llvm::Attribute::NoInline);
  Parent.addFnAttr(llvm::Attribute::OptimizeNone);
  // This protocol also keeps compiler intrinsics from resetting the current
  // asynchronous state, and makes the backend's state writes volatile.
  Parent.addFnAttr("llvm.rewrite.win-x86-registration-state");
  if (EH.Personality == ExceptionPersonality::ExceptHandler4 &&
      Chain.GSCookieOffset != -2)
    Parent.addFnAttr(llvm::Attribute::StackProtectReq);
  Parent.setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(
          *Ctx, {med_llvm_eh::mdUInt(*Ctx, 1, 1),
                 llvm::MDString::get(*Ctx, "seh-x86-registration-native")}));
  exception_rewrite::setContract(Parent,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::Complete,
                                 ProtectedCalls, ProtectedCalls, 0);
  return true;
}

} // namespace neverd

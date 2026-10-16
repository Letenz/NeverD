//===- COFFRegistrationSEHParentExit.cpp - Private parent context proof --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticates private parent context storage, transfers and resumption.

#include "COFFRegistrationSEHParentExit.h"

#include "COFFRegistrationCopySlots.h"

#include "neverd/Limits.h"

#include "llvm/IR/CFG.h"

#include <array>

namespace neverd::coff_registration {
namespace {

bool isCall(const llvm::CallInst *Call, llvm::Intrinsic::ID ID,
            unsigned Arguments, bool NoReturn = false) {
  if (!Call || Call->getIntrinsicID() != ID || Call->arg_size() != Arguments ||
      Call->getNumOperandBundles() || Call->isTailCall() ||
      Call->getCallingConv() != llvm::CallingConv::C)
    return false;
  const auto Attributes = Call->getAttributes();
  return NoReturn ? Attributes.getNumAttrSets() == 1 &&
                        Attributes.getFnAttrs().getNumAttributes() == 1 &&
                        Call->hasFnAttr(llvm::Attribute::NoReturn)
                  : Attributes.isEmpty();
}

bool isLoad(const llvm::LoadInst *Load, const llvm::Value *Pointer) {
  return Load && Load->getType()->isIntegerTy(32) && Load->isVolatile() &&
         !Load->isAtomic() && Load->getAlign() == llvm::Align(4) &&
         Load->getPointerOperand() == Pointer && Load->hasOneUse();
}

bool isStore(const llvm::StoreInst *Store, const llvm::Value *Pointer,
             const llvm::Value *Value) {
  return Store && Store->isVolatile() && !Store->isAtomic() &&
         Store->getAlign() == llvm::Align(4) &&
         Store->getPointerOperand() == Pointer &&
         Store->getValueOperand() == Value;
}

bool isHead(const llvm::Value *Value) {
  const auto *Pointer = llvm::dyn_cast<llvm::ConstantPointerNull>(Value);
  return Pointer && Pointer->getType()->getPointerAddressSpace() == 257;
}

} // namespace

llvm::Expected<RegistrationSEHParentExitContract> bindSEHParentExitContext(
    const llvm::Function &Parent, va_t Owner,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::set<const llvm::LoadInst *> &LocalUnwindChainLoads) {
  RegistrationSEHParentExitContract Result;
  std::map<const llvm::Function *, CallbackKey> CallbackOwners;
  std::vector<const llvm::Function *> CallbackFunctions;
  for (const auto &[Key, Function] : Callbacks) {
    CallbackOwners.emplace(Function, Key);
    CallbackFunctions.push_back(Function);
  }
  std::array<const llvm::AllocaInst *, 3> Slots{};
  const llvm::CallInst *Capture = nullptr;
  std::map<const llvm::CallInst *, va_t> Transfers;
  size_t Work = 0;
  for (const auto &Function : *Parent.getParent())
    for (const auto &Block : Function)
      for (const auto &I : Block) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("parent context closure exceeds its work budget");
        const auto *MD =
            I.getMetadata(windows_eh_md::RegistrationParentExitAttachment);
        if (!MD)
          continue;
        if (&Function != &Parent && !CallbackOwners.count(&Function) &&
            metadataInteger(*MD, 0, 64) != Owner)
          continue;
        const auto Identity = metadataInteger(*MD, 1, 64);
        const auto Role = metadataInteger(*MD, 2, 64);
        if (MD->getNumOperands() != 3 || metadataInteger(*MD, 0, 64) != Owner ||
            !Identity || !Role || *Role > 4)
          return rejectIR("parent context marker changed its identity");
        if (*Role < Slots.size()) {
          const auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (*Identity || Slots[*Role] || !Slot || !Slot->isStaticAlloca() ||
              Slot->isArrayAllocation() || Slot->isUsedWithInAlloca() ||
              Slot->getAddressSpace() || Slot->getAlign() != llvm::Align(4) ||
              &Block != &Parent.getEntryBlock())
            return rejectIR("parent context lost its private entry storage");
          Slots[*Role] = Slot;
        } else {
          const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
          if (*Role == 4) {
            if (*Identity || Capture || !Call ||
                &Block != &Parent.getEntryBlock())
              return rejectIR("parent context capture is not unique at entry");
            Capture = Call;
          } else {
            const auto Callback = CallbackOwners.find(&Function);
            if (!*Identity || !Call || Callback == CallbackOwners.end() ||
                !Callback->second.second)
              return rejectIR("parent transfer belongs to another callback");
            Transfers.emplace(Call, *Identity);
          }
        }
      }
  if (!Capture && Transfers.empty() && LocalUnwindChainLoads.empty() &&
      llvm::none_of(Slots, [](const auto *Slot) { return Slot != nullptr; }))
    return Result;
  if (!Capture || Transfers.empty() ||
      llvm::any_of(Slots, [](const auto *Slot) { return Slot == nullptr; }))
    return rejectIR("parent context has an incomplete capture/transfer set");
  const auto *ContextType =
      llvm::dyn_cast<llvm::ArrayType>(Slots[0]->getAllocatedType());
  if (!ContextType || ContextType->getNumElements() != 5 ||
      !ContextType->getElementType()->isPointerTy() ||
      ContextType->getElementType()->getPointerAddressSpace() ||
      !Slots[1]->getAllocatedType()->isIntegerTy(32) ||
      !Slots[2]->getAllocatedType()->isIntegerTy(32))
    return rejectIR("parent context storage changed its machine layout");
  const auto *Used = Parent.getParent()->getNamedGlobal("llvm.compiler.used");
  const auto *Kept =
      Used && Used->hasInitializer()
          ? llvm::dyn_cast<llvm::ConstantArray>(Used->getInitializer())
          : nullptr;
  if (!Used || !Used->hasAppendingLinkage() ||
      Used->getSection() != "llvm.metadata" || Used->isThreadLocal() ||
      Used->getAddressSpace() || !Kept ||
      llvm::any_of(CallbackFunctions, [&](const auto *Callback) {
        return llvm::none_of(Kept->operands(), [&](const llvm::Use &Value) {
          return Value->stripPointerCasts() == Callback;
        });
      }))
    return rejectIR(
        "parent transfer callbacks lost their retained machine ABI");

  RegistrationCopySlots Storage;
  if (!Storage.build(Parent, CallbackFunctions, Work))
    return rejectIR("parent context has an unowned frame recovery");
  std::array<std::set<const llvm::User *>, 3> Uses;
  auto IsAlias = [&](const llvm::Value *Value, unsigned Role) {
    return llvm::is_contained(Storage.aliases(Slots[Role]), Value);
  };
  const auto &Setup = Parent.getEntryBlock();
  std::vector<const llvm::Instruction *> Tail;
  for (const auto &I : llvm::reverse(Setup)) {
    if (!I.isDebugOrPseudoInst())
      Tail.push_back(&I);
    if (Tail.size() == 10)
      break;
  }
  if (Tail.size() != 10)
    return rejectIR("parent context has no contiguous capture sequence");
  std::reverse(Tail.begin(), Tail.end());
  const auto *Head = llvm::dyn_cast<llvm::LoadInst>(Tail[0]);
  const auto *SaveHead = llvm::dyn_cast<llvm::StoreInst>(Tail[1]);
  const auto *Frame = llvm::dyn_cast<llvm::CallInst>(Tail[2]);
  const auto *SaveFrame = llvm::dyn_cast<llvm::StoreInst>(Tail[3]);
  const auto *Stack = llvm::dyn_cast<llvm::CallInst>(Tail[4]);
  const auto *StackCell = llvm::dyn_cast<llvm::GetElementPtrInst>(Tail[5]);
  const auto *SaveStack = llvm::dyn_cast<llvm::StoreInst>(Tail[6]);
  const auto *Compare = llvm::dyn_cast<llvm::ICmpInst>(Tail[8]);
  const auto *Branch = llvm::dyn_cast<llvm::CondBrInst>(Tail[9]);
  if (!Head || !isHead(Head->getPointerOperand()) ||
      !isLoad(Head, Head->getPointerOperand()) ||
      !isStore(SaveHead, Slots[1], Head) ||
      !isCall(Frame, llvm::Intrinsic::frameaddress, 1) ||
      integer(Frame->getArgOperand(0), 32) != 0 || !Frame->hasOneUse() ||
      !isStore(SaveFrame, Slots[0], Frame) ||
      !isCall(Stack, llvm::Intrinsic::stacksave, 0) || !Stack->hasOneUse() ||
      !StackCell || StackCell->getPointerOperand() != Slots[0] ||
      !StackCell->getSourceElementType()->isIntegerTy(8) ||
      StackCell->getNumIndices() != 1 ||
      integer(StackCell->getOperand(1), 32) != 8 || !StackCell->hasOneUse() ||
      !isStore(SaveStack, StackCell, Stack) || Tail[7] != Capture ||
      !isCall(Capture, llvm::Intrinsic::eh_sjlj_setjmp, 1) ||
      Capture->getArgOperand(0) != Slots[0] || !Capture->hasOneUse() ||
      !Compare || Compare->getPredicate() != llvm::CmpInst::ICMP_EQ ||
      Compare->getOperand(0) != Capture ||
      integer(Compare->getOperand(1), 32) != 0 || !Compare->hasOneUse() ||
      !Branch || Branch->getCondition() != Compare ||
      Branch->getSuccessor(0) == Branch->getSuccessor(1))
    return rejectIR("parent context capture changed its stack or chain recipe");
  Result.Setup.insert(Tail.begin(), Tail.end());
  Result.ChainReads.insert(Head);
  Result.Calls.insert(Frame);
  Result.Calls.insert(Stack);
  Result.Calls.insert(Capture);
  Result.NormalEntry = Branch->getSuccessor(0);
  Uses[0] = {SaveFrame, StackCell, Capture};
  Uses[1] = {SaveHead};

  const auto *Resume = Branch->getSuccessor(1);
  const auto *Dispatch =
      llvm::dyn_cast<llvm::SwitchInst>(Resume->getTerminator());
  const auto *Selected = llvm::dyn_cast<llvm::LoadInst>(&Resume->front());
  if (Resume->size() != 2 || Resume->hasAddressTaken() ||
      Resume->getSinglePredecessor() != &Setup || !Dispatch ||
      !isLoad(Selected, Slots[2]) || Dispatch->getCondition() != Selected ||
      Dispatch->getNumCases() != Transfers.size())
    return rejectIR("parent continuation changed its private selector");
  const auto *Invalid = Dispatch->getDefaultDest();
  const auto *Trap = llvm::dyn_cast<llvm::CallInst>(&Invalid->front());
  if (Invalid->size() != 2 || Invalid->hasAddressTaken() ||
      Invalid->getSinglePredecessor() != Resume ||
      !isCall(Trap, llvm::Intrinsic::trap, 0) ||
      !llvm::isa<llvm::UnreachableInst>(Invalid->getTerminator()))
    return rejectIR("parent continuation admits an unowned destination");
  Uses[2].insert(Selected);
  Result.Dispatch = Dispatch;
  Result.Calls.insert(Trap);
  for (const auto &[Call, Address] : Transfers) {
    const auto *Block = Call->getParent();
    if (Block->size() != 5 || Block->hasAddressTaken())
      return rejectIR("parent transfer gained an unproved operation");
    auto It = Block->begin();
    const auto *Select = llvm::dyn_cast<llvm::StoreInst>(&*It++);
    const auto *Chain = llvm::dyn_cast<llvm::LoadInst>(&*It++);
    const auto *Restore = llvm::dyn_cast<llvm::StoreInst>(&*It++);
    if (!Select || !Select->getValueOperand()->getType()->isIntegerTy(32))
      return rejectIR("parent transfer has no finite source destination");
    const auto Id = integer(Select->getValueOperand(), 32);
    if (!Id || *Id > INT_MAX || !IsAlias(Select->getPointerOperand(), 2) ||
        !isStore(Select, Select->getPointerOperand(),
                 Select->getValueOperand()) ||
        !Chain || !IsAlias(Chain->getPointerOperand(), 1) ||
        !isLoad(Chain, Chain->getPointerOperand()) || !Restore ||
        !isHead(Restore->getPointerOperand()) ||
        !isStore(Restore, Restore->getPointerOperand(), Chain) ||
        &*It++ != Call ||
        !isCall(Call, llvm::Intrinsic::eh_sjlj_longjmp, 1, true) ||
        !IsAlias(Call->getArgOperand(0), 0) ||
        !llvm::isa<llvm::UnreachableInst>(&*It) ||
        !Result.Transfers
             .emplace(*Id,
                      RegistrationSEHParentExitContract::Transfer{
                          Block, CallbackOwners.at(Call->getFunction()).first,
                          Address})
             .second)
      return rejectIR(
          "parent transfer changed its stack, chain or destination");
    Uses[0].insert(Call);
    Uses[1].insert(Chain);
    Uses[2].insert(Select);
    Result.ChainWrites.insert(Restore);
    Result.Calls.insert(Call);
  }
  for (const auto &Case : Dispatch->cases()) {
    const auto Id = integer(Case.getCaseValue(), 32);
    if (!Id || *Id > INT_MAX || !Result.Transfers.count(*Id))
      return rejectIR("parent continuation has an unbound selector case");
    Result.ResumptionPredecessors[Case.getCaseSuccessor()].push_back(
        Result.Transfers.at(*Id).Block);
  }
  // The native stack, chain head and destination are compiler-only state.
  // A valid transfer does not authorize another observer or writer of any
  // alias, including a recovery from a function outside this callback set.
  for (const auto *Load : LocalUnwindChainLoads) {
    if (++Work > limits::kMaxRegistrationEHStateWork || !Load ||
        !CallbackOwners.count(Load->getFunction()) ||
        !CallbackOwners.at(Load->getFunction()).second ||
        !IsAlias(Load->getPointerOperand(), 1) ||
        !isLoad(Load, Load->getPointerOperand()))
      return rejectIR("local unwind borrowed another parent chain capture");
    Uses[1].insert(Load);
  }
  for (unsigned Role = 0; Role != Slots.size(); ++Role)
    for (const auto *Alias : Storage.aliases(Slots[Role]))
      for (const auto *User : Alias->users()) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("parent context use closure exceeds its work budget");
        if (Alias == Slots[Role] && Storage.isEscape(User))
          continue;
        if (!Uses[Role].count(User))
          return rejectIR(
              "private parent context has an unowned observer or write");
      }
  return Result;
}

} // namespace neverd::coff_registration

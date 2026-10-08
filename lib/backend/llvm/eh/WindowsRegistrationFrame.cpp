//===- WindowsRegistrationFrame.cpp - x86 runtime callback frames --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace neverd {
namespace {

llvm::Error reject(llvm::StringRef Detail) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "x86 registration callback: %s",
                                 Detail.str().c_str());
}

bool isStaticEntryAlloca(const llvm::AllocaInst &Slot,
                         const llvm::Function &Parent) {
  return Slot.getFunction() == &Parent && Slot.isStaticAlloca() &&
         Slot.getParent() == &Parent.getEntryBlock() &&
         Slot.getAddressSpace() == 0 && !Slot.isSwiftError() &&
         !Slot.isUsedWithInAlloca();
}

bool hasBlockAddress(const llvm::Constant *Value,
                     std::set<const llvm::Constant *> &Seen, size_t &WorkUsed,
                     unsigned Depth = 0) {
  if (++WorkUsed > limits::kMaxRegistrationEHStateWork || Depth > 256)
    return true;
  if (llvm::isa<llvm::BlockAddress>(Value))
    return true;
  if (!Seen.insert(Value).second)
    return false;
  if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value))
    return Global->hasInitializer() &&
           hasBlockAddress(Global->getInitializer(), Seen, WorkUsed, Depth + 1);
  if (const auto *Alias = llvm::dyn_cast<llvm::GlobalAlias>(Value))
    return hasBlockAddress(Alias->getAliasee(), Seen, WorkUsed, Depth + 1);
  if (llvm::isa<llvm::GlobalValue>(Value))
    return false;
  for (const llvm::Use &Operand : Value->operands())
    if (const auto *Constant = llvm::dyn_cast<llvm::Constant>(Operand.get());
        Constant && hasBlockAddress(Constant, Seen, WorkUsed, Depth + 1))
      return true;
  return false;
}

// Reconstruct a pointer into a recovered alloca. Arbitrary parent computations
// and memory reads are values at another execution point, not address recipes.
bool isEntryAddressRecipe(const llvm::Instruction &Instruction,
                          const llvm::Function &Parent) {
  if (Instruction.getParent() != &Parent.getEntryBlock())
    return false;
  if (llvm::isa<llvm::GetElementPtrInst>(Instruction) ||
      llvm::isa<llvm::CastInst>(Instruction))
    return true;
  if (Instruction.getOpcode() != llvm::Instruction::Add &&
      Instruction.getOpcode() != llvm::Instruction::Sub)
    return false;
  return llvm::isa<llvm::ConstantInt>(Instruction.getOperand(1));
}

struct CallbackPlan {
  const X86RegistrationCallbackRequest *Request = nullptr;
  std::set<llvm::AllocaInst *> Needed;
  std::set<llvm::Argument *> Arguments;
  std::vector<llvm::Instruction *> Recipes;
  std::map<llvm::StoreInst *, X86RegistrationRootKind> Seeds;
  std::set<llvm::AllocaInst *> PrivateSlots;
};

// A private stack is valid only for bounded scratch cells below entry ESP.
// Reject observable addresses, return-address reads and unproved SSA copies.
// The producer still owns the source entry-state proof; this checks the actual
// LLVM uses that would be redirected to the new allocation.
llvm::Error checkPrivateStack(
    const llvm::DataLayout &Layout, llvm::BasicBlock &Entry,
    const std::set<llvm::BasicBlock *> &Body,
    const std::map<llvm::StoreInst *, X86RegistrationRootKind> &Seeds,
    const X86RegistrationCallbackFrame &Frame,
    std::set<llvm::AllocaInst *> &PrivateSlots, size_t &WorkUsed) {
  std::map<llvm::Value *, int64_t> Offsets;
  std::vector<llvm::Value *> Work;
  struct ScratchAccess {
    int64_t Offset;
    uint64_t Size;
    bool Write;
  };
  std::map<llvm::Instruction *, ScratchAccess> Accesses;
  bool Invalid = false;
  bool Exhausted = false;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork - WorkUsed) {
      Exhausted = true;
      return false;
    }
    WorkUsed += Amount;
    return true;
  };
  auto Add = [&](llvm::Value *Value, int64_t Offset) {
    if (!Charge(1))
      return;
    auto [It, Inserted] = Offsets.emplace(Value, Offset);
    Invalid |= !Inserted && It->second != Offset;
    if (Inserted)
      Work.push_back(Value);
  };
  auto Loads = [&](llvm::AllocaInst *Slot) {
    for (llvm::User *User : Slot->users()) {
      if (!Charge(1))
        break;
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User))
        Add(Load, 0);
    }
  };
  for (const auto &[Definition, Kind] : Seeds)
    if (Kind == X86RegistrationRootKind::StackPointer)
      Loads(llvm::cast<llvm::AllocaInst>(Definition->getPointerOperand()));
  auto CheckAccess = [&](llvm::Type *Type, int64_t Offset, llvm::Align Align) {
    const llvm::TypeSize Size = Layout.getTypeStoreSize(Type);
    return !Size.isScalable() && Offset < 0 &&
           Offset >= -int64_t(*Frame.StackPointerOffset) &&
           Size.getFixedValue() <= uint64_t(-Offset) && Align.value() <= 16 &&
           (int64_t(*Frame.StackPointerOffset) + Offset) % Align.value() == 0;
  };
  while (!Work.empty() && !Invalid && !Exhausted) {
    if (!Charge(1))
      break;
    llvm::Value *Value = Work.back();
    Work.pop_back();
    const int64_t Offset = Offsets.at(Value);
    for (llvm::Use &Use : Value->uses()) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("private callback stack exceeds the work budget");
      auto *User = llvm::dyn_cast<llvm::Instruction>(Use.getUser());
      if (!User || !Body.count(User->getParent()))
        return reject("private callback stack address escapes its body");
      if (auto *Binary = llvm::dyn_cast<llvm::BinaryOperator>(User)) {
        const auto *Constant =
            llvm::dyn_cast<llvm::ConstantInt>(Binary->getOperand(1));
        if (!Constant || Use.getOperandNo() != 0 ||
            Binary->hasPoisonGeneratingFlags() ||
            (Binary->getOpcode() != llvm::Instruction::Add &&
             Binary->getOpcode() != llvm::Instruction::Sub) ||
            Constant->getBitWidth() > 64)
          return reject(
              "private callback stack address has unproved arithmetic");
        const int64_t Delta = Constant->getSExtValue();
        // Keep the proof in the bounded signed PE32 displacement domain.
        if (Delta < -int64_t(UINT32_MAX) || Delta > int64_t(UINT32_MAX))
          return reject("private callback stack displacement is unbounded");
        const int64_t Next =
            Offset +
            (Binary->getOpcode() == llvm::Instruction::Add ? Delta : -Delta);
        if (Next < -int64_t(Frame.StackBytes) || Next > Frame.StackBytes)
          return reject("private callback stack displacement is out of bounds");
        Add(User, Next);
        continue;
      }
      if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(User)) {
        auto IsFullAddress = [](llvm::Type *Type) {
          if (const auto *Pointer = llvm::dyn_cast<llvm::PointerType>(Type))
            return Pointer->getAddressSpace() == 0;
          return Type->isIntegerTy(32) || Type->isIntegerTy(64);
        };
        if (Cast->hasPoisonGeneratingFlags() ||
            !IsFullAddress(Cast->getSrcTy()) ||
            !IsFullAddress(Cast->getDestTy()))
          return reject(
              "private callback stack has an unsupported address cast");
        Add(User, Offset);
        continue;
      }
      if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(User)) {
        llvm::APInt Delta(32, 0);
        if (Use.getOperandNo() != 0 || GEP->getPointerAddressSpace() != 0 ||
            !GEP->accumulateConstantOffset(Layout, Delta))
          return reject("private callback stack has an unproved GEP");
        const int64_t Next = Offset + Delta.getSExtValue();
        if (Next < -int64_t(Frame.StackBytes) || Next > Frame.StackBytes)
          return reject("private callback stack GEP is out of bounds");
        if (GEP->hasNoUnsignedWrap() ||
            ((GEP->isInBounds() || GEP->hasNoUnsignedSignedWrap()) &&
             (int64_t(*Frame.StackPointerOffset) + Next < 0 ||
              int64_t(*Frame.StackPointerOffset) + Next > Frame.StackBytes)))
          return reject(
              "private callback stack GEP has unproved no-wrap bounds");
        Add(User, Next);
        continue;
      }
      if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
        if (Use.getOperandNo() != 0 ||
            !CheckAccess(Load->getType(), Offset, Load->getAlign()))
          return reject("private callback stack reads an unproved entry cell");
        Accesses.emplace(
            Load, ScratchAccess{
                      Offset,
                      Layout.getTypeStoreSize(Load->getType()).getFixedValue(),
                      false});
        continue;
      }
      if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(User)) {
        if (Use.getOperandNo() == 1) {
          if (!CheckAccess(Store->getValueOperand()->getType(), Offset,
                           Store->getAlign()))
            return reject("private callback stack write is out of bounds");
          Accesses.emplace(
              Store,
              ScratchAccess{
                  Offset,
                  Layout.getTypeStoreSize(Store->getValueOperand()->getType())
                      .getFixedValue(),
                  true});
          continue;
        }
        auto *Slot =
            llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand());
        if (!Slot || !Slot->isStaticAlloca() ||
            (!Slot->getAllocatedType()->isIntegerTy(32) &&
             !Slot->getAllocatedType()->isIntegerTy(64) &&
             !Slot->getAllocatedType()->isPointerTy()) ||
            !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne() ||
            Store->getValueOperand()->getType() != Slot->getAllocatedType() ||
            Store->isVolatile() || Store->isAtomic())
          return reject("private callback stack address is stored externally");
        for (llvm::User *SlotUser : Slot->users()) {
          if (!Charge(1))
            return reject(
                "private callback stack SSA uses exceed the work budget");
          auto *Load = llvm::dyn_cast<llvm::LoadInst>(SlotUser);
          if (SlotUser == Store)
            continue;
          if (!Load || Load->getType() != Slot->getAllocatedType() ||
              !Body.count(Load->getParent()) || Load->isVolatile() ||
              Load->isAtomic() ||
              (Store->getParent() != &Entry &&
               Store->getParent() != Load->getParent()) ||
              (Store->getParent() == Load->getParent() &&
               !Store->comesBefore(Load)))
            return reject("private callback stack SSA copy has unproved uses");
          Add(Load, Offset);
        }
        PrivateSlots.insert(Slot);
        continue;
      }
      return reject("private callback stack address has an observable use");
    }
  }
  if (Exhausted)
    return reject("private callback stack values exceed the work budget");
  if (Invalid)
    return reject("private callback stack has inconsistent SSA offsets");

  // Memory below the dispatcher's incoming ESP is not automatically scratch:
  // a read must be preceded by writes on every callback path. Solve definite
  // byte initialization from the callback root, including backedges.
  std::set<int64_t> Universe;
  for (const auto &[Instruction, Access] : Accesses)
    for (uint64_t I = 0; I < Access.Size; ++I) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("private callback scratch proof exceeds the work budget");
      Universe.insert(Access.Offset + I);
    }
  std::vector<llvm::BasicBlock *> OrderedBlocks;
  std::map<llvm::BasicBlock *, std::vector<ScratchAccess>> OrderedAccesses;
  std::map<llvm::BasicBlock *, std::set<int64_t>> Incoming, Outgoing;
  std::deque<llvm::BasicBlock *> Pending;
  std::set<llvm::BasicBlock *> Queued;
  for (llvm::BasicBlock &Block : *Entry.getParent()) {
    if (!Charge(1))
      return reject(
          "private callback scratch block scan exceeds the work budget");
    if (!Body.count(&Block))
      continue;
    OrderedBlocks.push_back(&Block);
    for (llvm::Instruction &Instruction : Block) {
      if (!Charge(1))
        return reject("private callback scratch instruction scan exceeds the "
                      "work budget");
      if (auto It = Accesses.find(&Instruction); It != Accesses.end())
        OrderedAccesses[&Block].push_back(It->second);
    }
    if (!Charge(Universe.size()))
      return reject("private callback scratch state exceeds the work budget");
    Outgoing[&Block] = Universe;
    Pending.push_back(&Block);
    Queued.insert(&Block);
  }
  while (!Pending.empty()) {
    if (!Charge(1))
      return reject(
          "private callback scratch worklist exceeds the work budget");
    llvm::BasicBlock *Block = Pending.front();
    Pending.pop_front();
    Queued.erase(Block);
    std::set<int64_t> In;
    if (Block != &Entry) {
      if (!Charge(Universe.size()))
        return reject(
            "private callback scratch state copy exceeds the work budget");
      In = Universe;
      for (llvm::BasicBlock *Pred : llvm::predecessors(Block)) {
        if (!Charge(1))
          return reject(
              "private callback scratch predecessors exceed the work budget");
        for (auto It = In.begin(); It != In.end();) {
          if (!Charge(1))
            return reject(
                "private callback scratch flow exceeds the work budget");
          if (!Outgoing.at(Pred).count(*It))
            It = In.erase(It);
          else
            ++It;
        }
      }
    }
    if (!Charge(In.size()))
      return reject(
          "private callback scratch incoming copy exceeds the work budget");
    Incoming[Block] = In;
    for (const ScratchAccess &Access : OrderedAccesses[Block]) {
      if (!Charge(1 + (Access.Write ? Access.Size : 0)))
        return reject("private callback scratch stores exceed the work budget");
      if (Access.Write)
        for (uint64_t I = 0; I < Access.Size; ++I)
          In.insert(Access.Offset + I);
    }
    if (!Charge(In.size() + Outgoing[Block].size()))
      return reject(
          "private callback scratch comparison exceeds the work budget");
    if (Outgoing[Block] != In) {
      Outgoing[Block] = std::move(In);
      for (llvm::BasicBlock *Successor : llvm::successors(Block)) {
        if (!Charge(1))
          return reject(
              "private callback scratch successors exceed the work budget");
        if (Queued.insert(Successor).second)
          Pending.push_back(Successor);
      }
    }
  }
  for (llvm::BasicBlock *Block : OrderedBlocks) {
    if (!Charge(1 + Incoming.at(Block).size()))
      return reject(
          "private callback scratch final state exceeds the work budget");
    auto Initialized = Incoming.at(Block);
    for (const ScratchAccess &Access : OrderedAccesses[Block]) {
      if (!Charge(1 + Access.Size))
        return reject("private callback scratch reads exceed the work budget");
      for (uint64_t I = 0; I < Access.Size; ++I)
        if (Access.Write)
          Initialized.insert(Access.Offset + I);
        else if (!Initialized.count(Access.Offset + I))
          return reject(
              "private callback scratch read is not definitely initialized");
    }
  }
  return llvm::Error::success();
}

llvm::Expected<CallbackPlan>
prepareCallback(llvm::Function &Parent,
                const X86RegistrationCallbackRequest &Request,
                size_t &WorkUsed) {
  if (!Request.Entry)
    return reject("callback has no entry");
  llvm::BasicBlock &Entry = *Request.Entry;
  llvm::ArrayRef<llvm::BasicBlock *> Blocks = Request.Blocks;
  const X86RegistrationCallbackKind Kind = Request.Kind;
  if (Kind != X86RegistrationCallbackKind::Filter &&
      Kind != X86RegistrationCallbackKind::Finally)
    return reject("unknown callback kind");
  llvm::StringRef Name = Request.Name;
  const X86RegistrationCallbackFrame &Frame = Request.Frame;
  llvm::Module *Module = Parent.getParent();
  if (!Module || Parent.empty() || !Parent.getEntryBlock().getTerminator())
    return reject("parent has no complete entry block");
  if (Parent.hasGC())
    return reject("callback depends on a parent GC strategy");
  const llvm::Triple Triple(Module->getTargetTriple());
  const llvm::DataLayout &Layout = Module->getDataLayout();
  if (Triple.getArch() != llvm::Triple::x86 || !Triple.isOSWindows() ||
      !Triple.isOSBinFormatCOFF() || Layout.getPointerSize(0) != 4)
    return reject("requires the PE32 Windows x86 ABI");
  if (Name.empty() || Module->getNamedValue(Name))
    return reject("callback name is empty or already owned");
  const auto *Personality =
      Parent.hasPersonalityFn()
          ? llvm::dyn_cast<llvm::Function>(
                Parent.getPersonalityFn()->stripPointerCasts())
          : nullptr;
  if (!Personality ||
      (Personality->getName() != "_except_handler3" &&
       Personality->getName() != "_except_handler4") ||
      !Personality->isDeclaration() || !Personality->hasExternalLinkage() ||
      Personality->getCallingConv() != llvm::CallingConv::C ||
      !Personality->getReturnType()->isIntegerTy(32) ||
      !Personality->isVarArg() || Personality->arg_size() != 0)
    return reject("parent has no authenticated EH3/EH4 personality");
  if (Blocks.empty() || Blocks.size() > limits::kMaxRegistrationEHStateWork)
    return reject("callback block set is empty or exceeds the work budget");
  std::set<llvm::BasicBlock *> Body;
  for (llvm::BasicBlock *Block : Blocks) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
      return reject("callback block identities exceed the work budget");
    if (!Block || Block->getParent() != &Parent ||
        Block == &Parent.getEntryBlock() || !Block->getTerminator() ||
        !Body.insert(Block).second)
      return reject("callback blocks do not have unique parent identities");
  }
  if (!Body.count(&Entry))
    return reject("callback entry is outside its body");
  for (llvm::BasicBlock *Block : Blocks) {
    if (Block->hasAddressTaken())
      return reject("callback block address escapes its outlined body");
    for (llvm::User *User : Block->users()) {
      auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (!Instruction || !Body.count(Instruction->getParent()))
        return reject("callback block has a user outside its outlined body");
    }
  }
  if (!Entry.phis().empty())
    return reject("callback entry PHIs have no proven runtime incoming value");

  std::set<llvm::BasicBlock *> Reached;
  std::vector<llvm::BasicBlock *> Work{&Entry};
  while (!Work.empty()) {
    llvm::BasicBlock *Block = Work.back();
    Work.pop_back();
    if (!Reached.insert(Block).second)
      continue;
    for (llvm::BasicBlock *Pred : llvm::predecessors(Block))
      if (!Body.count(Pred))
        return reject("ordinary parent flow enters the callback");
    for (llvm::BasicBlock *Succ : llvm::successors(Block)) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback traversal exceeds the work budget");
      if (!Body.count(Succ))
        return reject("callback flow leaves its outlined body");
      Work.push_back(Succ);
    }
  }
  if (Reached != Body)
    return reject("callback contains an unreachable block");

  std::map<llvm::StoreInst *, X86RegistrationRootKind> Seeds;
  std::set<llvm::AllocaInst *> PrivateSlots;
  bool NeedsFP = false;
  bool NeedsSP = false;
  if (Frame.RootSeeds.size() > limits::kMaxRegistrationEHRecords)
    return reject("callback runtime seeds exceed the work budget");
  for (const X86RegistrationRootSeed &Seed : Frame.RootSeeds) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
      return reject("callback runtime seed checks exceed the work budget");
    llvm::StoreInst *Definition = Seed.Definition;
    if (!Definition || Definition->getParent() != &Entry ||
        Definition->isVolatile() || Definition->isAtomic() ||
        !Definition->getValueOperand()->getType()->isIntegerTy(32) ||
        !Seeds.emplace(Definition, Seed.Kind).second)
      return reject(
          "runtime register seed is not a unique entry i32 definition");
    auto *Slot =
        llvm::dyn_cast<llvm::AllocaInst>(Definition->getPointerOperand());
    if (!Slot || !isStaticEntryAlloca(*Slot, Parent) ||
        !Slot->getAllocatedType()->isIntegerTy(32) ||
        !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne())
      return reject("runtime register seed does not define a scalar SSA slot");
    if (!PrivateSlots.insert(Slot).second)
      return reject("runtime register seeds share a destination slot");
    for (llvm::User *User : Slot->users()) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback runtime seed uses exceed the work budget");
      if (User == Definition)
        continue;
      auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
      if (!Load || Load->getType() != Slot->getAllocatedType() ||
          !Body.count(Load->getParent()) || Load->isVolatile() ||
          Load->isAtomic() ||
          (Load->getParent() == &Entry && !Definition->comesBefore(Load)))
        return reject(
            "runtime register seed has unproved storage or dominance");
    }
    switch (Seed.Kind) {
    case X86RegistrationRootKind::FramePointer:
      NeedsFP = true;
      break;
    case X86RegistrationRootKind::StackPointer:
      NeedsSP = true;
      break;
    default:
      return reject("unknown runtime register seed kind");
    }
  }
  if (NeedsSP != (Frame.StackBytes != 0) ||
      NeedsSP != Frame.StackPointerOffset.has_value() ||
      Frame.StackBytes > limits::kMaxRegistrationEHStateWork ||
      (NeedsSP && *Frame.StackPointerOffset > Frame.StackBytes))
    return reject(
        "private callback stack has no proven bounded size and anchor");
  if (NeedsFP != Frame.EstablishedFramePointerOffset.has_value())
    return reject("source EBP seed has no proven synthetic-frame offset");
  const bool HasExceptionCell = Frame.ExceptionPointersOffset.has_value();
  if (HasExceptionCell && Kind != X86RegistrationCallbackKind::Filter)
    return reject("exception-pointer cell is not a filter cell");
  if ((HasExceptionCell || NeedsFP) != (Frame.SyntheticFrame != nullptr))
    return reject("source frame bindings are incomplete");
  if (Frame.SyntheticFrame) {
    llvm::AllocaInst &Slot = *Frame.SyntheticFrame;
    if (!isStaticEntryAlloca(Slot, Parent))
      return reject("synthetic frame is not a shared entry alloca");
    const auto Bytes = Slot.getAllocationSize(Layout);
    if (!Bytes || Bytes->isScalable() || Bytes->getFixedValue() == 0 ||
        Bytes->getFixedValue() > std::numeric_limits<uint32_t>::max())
      return reject("synthetic frame has no bounded PE32 allocation");
    if (HasExceptionCell &&
        (Bytes->getFixedValue() < 4 ||
         *Frame.ExceptionPointersOffset > Bytes->getFixedValue() - 4))
      return reject("exception-pointer cell lies outside the synthetic frame");
    if (NeedsFP &&
        *Frame.EstablishedFramePointerOffset >= Bytes->getFixedValue())
      return reject("source EBP lies outside the synthetic frame");
  }
  if (NeedsSP)
    if (llvm::Error Error = checkPrivateStack(Layout, Entry, Body, Seeds, Frame,
                                              PrivateSlots, WorkUsed))
      return std::move(Error);

  std::set<llvm::AllocaInst *> Needed;
  std::set<llvm::Argument *> Arguments;
  std::vector<llvm::Instruction *> Recipes;
  std::set<llvm::Instruction *> Visiting;
  std::set<llvm::Instruction *> Visited;
  std::set<const llvm::Constant *> Constants;
  bool InvalidDependency = false;
  std::function<void(llvm::Value *, unsigned)> Inspect = [&](llvm::Value *Value,
                                                             unsigned Depth) {
    if (++WorkUsed > limits::kMaxRegistrationEHStateWork || Depth > 256) {
      InvalidDependency = true;
      return;
    }
    if (auto *Constant = llvm::dyn_cast<llvm::Constant>(Value)) {
      InvalidDependency |= hasBlockAddress(Constant, Constants, WorkUsed);
      return;
    }
    if (auto *Argument = llvm::dyn_cast<llvm::Argument>(Value)) {
      if (Argument->getParent() != &Parent || !Argument->getType()->isSized())
        InvalidDependency = true;
      else
        Arguments.insert(Argument);
      return;
    }
    if (auto *Block = llvm::dyn_cast<llvm::BasicBlock>(Value)) {
      InvalidDependency |= !Body.count(Block);
      return;
    }
    auto *Instruction = llvm::dyn_cast<llvm::Instruction>(Value);
    if (!Instruction) {
      InvalidDependency = true;
      return;
    }
    if (Body.count(Instruction->getParent()))
      return;
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Instruction)) {
      if (!isStaticEntryAlloca(*Slot, Parent))
        InvalidDependency = true;
      else if (!PrivateSlots.count(Slot))
        Needed.insert(Slot);
      return;
    }
    if (Visited.count(Instruction))
      return;
    if (!isEntryAddressRecipe(*Instruction, Parent) ||
        !Visiting.insert(Instruction).second) {
      InvalidDependency = true;
      return;
    }
    for (llvm::Value *Operand : Instruction->operands())
      Inspect(Operand, Depth + 1);
    Visiting.erase(Instruction);
    Visited.insert(Instruction);
    Recipes.push_back(Instruction);
  };

  bool HasReturn = false;
  for (llvm::BasicBlock *Block : Blocks)
    for (llvm::Instruction &Instruction : *Block) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("callback instructions exceed the work budget");
      if (Instruction.isEHPad() || llvm::isa<llvm::InvokeInst>(Instruction) ||
          llvm::isa<llvm::CallBrInst>(Instruction) ||
          llvm::isa<llvm::IndirectBrInst>(Instruction) ||
          llvm::isa<llvm::ResumeInst>(Instruction) ||
          llvm::isa<llvm::AllocaInst>(Instruction))
        return reject("callback contains an unsupported entry or control form");
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
        if ((llvm::isa<llvm::CallInst>(Call) &&
             llvm::cast<llvm::CallInst>(Call)->isMustTailCall()) ||
            Call->isInlineAsm() || Call->hasOperandBundles() ||
            Call->hasFnAttr(llvm::Attribute::ReturnsTwice))
          return reject("callback call depends on its original ABI context");
      if (const auto *Intrinsic =
              llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction))
        switch (Intrinsic->getIntrinsicID()) {
        case llvm::Intrinsic::frameaddress:
        case llvm::Intrinsic::returnaddress:
        case llvm::Intrinsic::localaddress:
        case llvm::Intrinsic::localescape:
        case llvm::Intrinsic::localrecover:
        case llvm::Intrinsic::stacksave:
        case llvm::Intrinsic::stackrestore:
        case llvm::Intrinsic::vastart:
        case llvm::Intrinsic::vaend:
        case llvm::Intrinsic::vacopy:
        case llvm::Intrinsic::experimental_deoptimize:
          return reject(
              "callback intrinsic depends on its original frame or signature");
        default:
          break;
        }
      if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        HasReturn = true;
        if (Kind == X86RegistrationCallbackKind::Filter &&
            (!Return->getReturnValue() ||
             !Return->getReturnValue()->getType()->isIntegerTy(32)))
          return reject("filter does not return an exact i32 result");
      }
      for (llvm::Use &Operand : Instruction.operands()) {
        if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
            Store && Seeds.count(Store) && Operand.getOperandNo() == 0)
          continue;
        Inspect(Operand.get(), 0);
      }
    }
  if (!HasReturn || InvalidDependency)
    return reject("callback return or parent-value dependencies are unproven");
  if (Frame.SyntheticFrame)
    Needed.insert(Frame.SyntheticFrame);
  return CallbackPlan{
      &Request,           std::move(Needed), std::move(Arguments),
      std::move(Recipes), std::move(Seeds),  std::move(PrivateSlots)};
}

llvm::Function *
commitCallback(llvm::Function &Parent, const CallbackPlan &Plan,
               llvm::ArrayRef<llvm::AllocaInst *> Escaped,
               const std::map<llvm::Argument *, uint32_t> &ArgumentIndices) {
  llvm::Module *Module = Parent.getParent();
  const auto &Request = *Plan.Request;
  llvm::BasicBlock &Entry = *Request.Entry;
  llvm::ArrayRef<llvm::BasicBlock *> Blocks = Request.Blocks;
  const auto Kind = Request.Kind;
  llvm::StringRef Name = Request.Name;
  const auto &Frame = Request.Frame;
  const auto &Needed = Plan.Needed;
  const auto &Recipes = Plan.Recipes;
  const bool HasExceptionCell = Frame.ExceptionPointersOffset.has_value();
  // Commit: all operands, closure, ABI shapes, and bounds are now checked.
  llvm::LLVMContext &Context = Module->getContext();
  auto *PointerTy = llvm::PointerType::get(Context, 0);
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *I8Ty = llvm::Type::getInt8Ty(Context);
  auto *CallbackTy =
      Kind == X86RegistrationCallbackKind::Filter
          ? llvm::FunctionType::get(I32Ty, false)
          : llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                    {I8Ty, PointerTy}, false);
  llvm::Function *Callback = llvm::Function::Create(
      CallbackTy, llvm::GlobalValue::InternalLinkage, Name, Module);
  Callback->addFnAttr(llvm::Attribute::NoInline);
  Callback->addFnAttr(llvm::Attribute::OptimizeNone);
  for (llvm::Attribute::AttrKind Kind :
       {llvm::Attribute::NullPointerIsValid, llvm::Attribute::StrictFP})
    if (Parent.hasFnAttribute(Kind))
      Callback->addFnAttr(Kind);
  for (llvm::StringRef Name : {"target-cpu", "target-features", "tune-cpu",
                               "denormal-fp-math", "denormal-fp-math-f32"})
    if (Parent.hasFnAttribute(Name))
      Callback->addFnAttr(Parent.getFnAttribute(Name));
  Callback->addFnAttr("frame-pointer", "all");
  Parent.addFnAttr("frame-pointer", "all");
  auto *Setup = llvm::BasicBlock::Create(Context, "recover.frame", Callback);
  llvm::IRBuilder<> Builder(Setup);
  llvm::Value *RuntimeFrame = nullptr;
  llvm::Value *ParentFrame = nullptr;
  if (Kind == X86RegistrationCallbackKind::Filter) {
    RuntimeFrame = Builder.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(
            Module, llvm::Intrinsic::frameaddress, {PointerTy}),
        {Builder.getInt32(1)}, "registration.frame");
    ParentFrame = Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                         Module, llvm::Intrinsic::eh_recoverfp),
                                     {&Parent, RuntimeFrame}, "parent.frame");
  } else {
    Callback->getArg(0)->setName("abnormal");
    Callback->getArg(0)->addAttr(llvm::Attribute::ZExt);
    Callback->getArg(1)->setName("parent.frame");
    ParentFrame = Callback->getArg(1);
  }
  llvm::ValueToValueMapTy Values;
  // Root register storage and scratch-address SSA copies belong to this
  // callback invocation, never to the interrupted parent frame.
  for (llvm::Instruction &Instruction : Parent.getEntryBlock())
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        Slot && Plan.PrivateSlots.count(Slot)) {
      auto *Private =
          Builder.CreateAlloca(Slot->getAllocatedType(), Slot->getArraySize(),
                               Slot->getName() + ".private");
      Private->setAlignment(Slot->getAlign());
      Values[Slot] = Private;
    }
  llvm::Function *Recover = llvm::Intrinsic::getOrInsertDeclaration(
      Module, llvm::Intrinsic::localrecover);
  for (size_t Index = 0; Index < Escaped.size(); ++Index)
    if (llvm::AllocaInst *Slot = Escaped[Index]; Needed.count(Slot))
      Values[Slot] = Builder.CreateCall(
          Recover, {&Parent, ParentFrame, Builder.getInt32(Index)},
          Slot->getName() + ".recovered");

  for (llvm::Argument &Argument : Parent.args())
    if (Plan.Arguments.count(&Argument)) {
      const uint32_t Index = ArgumentIndices.at(&Argument);
      llvm::Value *Address = Builder.CreateCall(
          Recover, {&Parent, ParentFrame, Builder.getInt32(Index)},
          "argument.recovered");
      Values[&Argument] =
          Builder.CreateLoad(Argument.getType(), Address, Argument.getName());
    }
  llvm::Value *SP = nullptr;
  if (Frame.StackBytes) {
    auto *Stack =
        Builder.CreateAlloca(llvm::ArrayType::get(I8Ty, Frame.StackBytes),
                             nullptr, "callback.stack");
    Stack->setAlignment(llvm::Align(16));
    llvm::Value *Top = Builder.CreateInBoundsGEP(
        I8Ty, Stack, Builder.getInt32(*Frame.StackPointerOffset),
        "callback.sp");
    SP = Builder.CreatePtrToInt(Top, I32Ty);
  }
  llvm::Value *FP = nullptr;
  if (Frame.EstablishedFramePointerOffset) {
    llvm::Value *Address = Builder.CreateInBoundsGEP(
        I8Ty, Values[Frame.SyntheticFrame],
        Builder.getInt32(*Frame.EstablishedFramePointerOffset), "source.ebp");
    FP = Builder.CreatePtrToInt(Address, I32Ty);
  }
  if (HasExceptionCell) {
    // EH3/EH4's dispatcher passes EBP = registration + 16 to a filter. Its
    // EXCEPTION_POINTERS cell is one word before the registration, at EBP-20.
    llvm::Value *RuntimeCell = Builder.CreateGEP(
        I8Ty, RuntimeFrame, Builder.getInt32(-20), "runtime.exception.cell");
    llvm::Value *Pointers =
        Builder.CreateLoad(PointerTy, RuntimeCell, "exception.pointers");
    llvm::Value *LogicalCell = Builder.CreateInBoundsGEP(
        I8Ty, Values[Frame.SyntheticFrame],
        Builder.getInt32(*Frame.ExceptionPointersOffset),
        "source.exception.cell");
    Builder.CreateStore(Pointers, LogicalCell)->setAlignment(llvm::Align(1));
  }
  for (llvm::Instruction *Recipe : Recipes) {
    llvm::Instruction *Clone = Recipe->clone();
    llvm::RemapInstruction(Clone, Values, llvm::RF_NoModuleLevelChanges);
    Builder.Insert(Clone, Recipe->getName());
    Clone->setDebugLoc({});
    Values[Recipe] = Clone;
  }
  for (llvm::BasicBlock *Block : Blocks)
    Values[Block] = llvm::CloneBasicBlock(Block, Values, ".outlined", Callback);
  // Replace exact entry seed values before remapping the cloned instructions.
  // A seed's old value need not be a valid cross-function dependency; it is
  // precisely the implicit runtime live-in that this binding defines.
  for (const auto &[Definition, Kind] : Plan.Seeds) {
    auto *Clone = llvm::cast<llvm::StoreInst>(Values[Definition]);
    Clone->setOperand(0,
                      Kind == X86RegistrationRootKind::FramePointer ? FP : SP);
  }
  for (llvm::BasicBlock *Block : Blocks) {
    auto *Clone = llvm::cast<llvm::BasicBlock>(Values[Block]);
    for (llvm::Instruction &Instruction : *Clone) {
      llvm::RemapInstruction(&Instruction, Values,
                             llvm::RF_NoModuleLevelChanges);
      Instruction.setDebugLoc({});
      Instruction.setMetadata(language_eh_md::InternalSourceCallAttachment,
                              nullptr);
    }
    if (Kind == X86RegistrationCallbackKind::Finally)
      if (auto *Return =
              llvm::dyn_cast<llvm::ReturnInst>(Clone->getTerminator())) {
        llvm::IRBuilder<>(Return).CreateRetVoid();
        Return->eraseFromParent();
      }
  }
  Builder.CreateBr(llvm::cast<llvm::BasicBlock>(Values[&Entry]));
  return Callback;
}

} // namespace

llvm::Expected<std::vector<llvm::Function *>> outlineX86RegistrationCallbacks(
    llvm::Function &Parent,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests) {
  if (Requests.empty() || Requests.size() > limits::kMaxRegistrationEHRecords)
    return reject("callback batch is empty or exceeds the work budget");
  size_t WorkUsed = 0;
  std::set<std::string> Names;
  std::set<llvm::BasicBlock *> OwnedBlocks;
  std::vector<CallbackPlan> Plans;
  std::set<llvm::AllocaInst *> Needed;
  std::set<llvm::Argument *> Arguments;
  for (const auto &Request : Requests) {
    if (!Names.insert(Request.Name).second)
      return reject("callback batch contains duplicate names");
    for (llvm::BasicBlock *Block : Request.Blocks)
      if (!OwnedBlocks.insert(Block).second)
        return reject("callback batch shares source blocks");
    auto Plan = prepareCallback(Parent, Request, WorkUsed);
    if (!Plan)
      return Plan.takeError();
    Needed.insert(Plan->Needed.begin(), Plan->Needed.end());
    Arguments.insert(Plan->Arguments.begin(), Plan->Arguments.end());
    Plans.push_back(std::move(*Plan));
  }
  llvm::CallInst *ExistingEscape = nullptr;
  std::vector<llvm::AllocaInst *> Escaped;
  std::set<llvm::AllocaInst *> AlreadyEscaped;
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block) {
      if (++WorkUsed > limits::kMaxRegistrationEHStateWork)
        return reject("parent escape scan exceeds the work budget");
      if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
          Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape) {
        if (ExistingEscape || &Block != &Parent.getEntryBlock())
          return reject("parent localescape is duplicated or outside entry");
        ExistingEscape = Call;
        for (llvm::Value *Operand : Call->args()) {
          auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Operand);
          if (!Slot || !isStaticEntryAlloca(*Slot, Parent))
            return reject("existing localescape does not name a static alloca");
          Escaped.push_back(Slot);
          AlreadyEscaped.insert(Slot);
        }
      }
    }
  for (llvm::Instruction &Instruction : Parent.getEntryBlock())
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        Slot && Needed.count(Slot) && AlreadyEscaped.insert(Slot).second)
      Escaped.push_back(Slot);
  if (Escaped.size() + Arguments.size() > limits::kMaxRegistrationEHStateWork)
    return reject("escaped callback frame exceeds the work budget");

  // Commit one parent frame for the entire preflighted callback set. Argument
  // stores precede all original operations that could invoke a callback.
  llvm::IRBuilder<> EntryBuilder(&Parent.getEntryBlock(),
                                 Parent.getEntryBlock().getFirstInsertionPt());
  std::map<llvm::Argument *, uint32_t> ArgumentIndices;
  for (llvm::Argument &Argument : Parent.args())
    if (Arguments.count(&Argument)) {
      auto *Slot =
          EntryBuilder.CreateAlloca(Argument.getType(), nullptr, "eh.argument");
      EntryBuilder.CreateStore(&Argument, Slot);
      ArgumentIndices.emplace(&Argument, Escaped.size());
      Escaped.push_back(Slot);
    }
  if (!Escaped.empty() || ExistingEscape) {
    llvm::IRBuilder<> EscapeBuilder(Parent.getEntryBlock().getTerminator());
    llvm::SmallVector<llvm::Value *, 8> Operands(Escaped.begin(),
                                                 Escaped.end());
    EscapeBuilder.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(Parent.getParent(),
                                                llvm::Intrinsic::localescape),
        Operands);
    if (ExistingEscape)
      ExistingEscape->eraseFromParent();
  }
  std::vector<llvm::Function *> Callbacks;
  for (const CallbackPlan &Plan : Plans)
    Callbacks.push_back(commitCallback(Parent, Plan, Escaped, ArgumentIndices));
  return Callbacks;
}

llvm::Expected<llvm::Function *> outlineX86RegistrationCallback(
    llvm::Function &Parent, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Blocks, X86RegistrationCallbackKind Kind,
    llvm::StringRef Name, const X86RegistrationCallbackFrame &Frame) {
  X86RegistrationCallbackRequest Request;
  Request.Entry = &Entry;
  Request.Blocks.assign(Blocks.begin(), Blocks.end());
  Request.Kind = Kind;
  Request.Name = Name.str();
  Request.Frame = Frame;
  auto Result = outlineX86RegistrationCallbacks(Parent, {Request});
  if (!Result)
    return Result.takeError();
  return Result->front();
}

} // namespace neverd

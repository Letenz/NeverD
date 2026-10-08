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
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

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
         Slot.getAddressSpace() == 0;
}

bool hasBlockAddress(const llvm::Constant *Value,
                     std::set<const llvm::Constant *> &Seen, size_t &WorkUsed,
                     unsigned Depth = 0) {
  if (++WorkUsed > limits::kMaxRegistrationEHStateWork || Depth > 256)
    return true;
  if (llvm::isa<llvm::BlockAddress>(Value))
    return true;
  if (llvm::isa<llvm::GlobalValue>(Value) || !Seen.insert(Value).second)
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

} // namespace

llvm::Expected<llvm::Function *> outlineX86RegistrationCallback(
    llvm::Function &Parent, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Blocks, X86RegistrationCallbackKind Kind,
    llvm::StringRef Name, const X86RegistrationCallbackFrame &Frame) {
  llvm::Module *Module = Parent.getParent();
  if (!Module || Parent.empty() || !Parent.getEntryBlock().getTerminator())
    return reject("parent has no complete entry block");
  const llvm::Triple Triple(Module->getTargetTriple());
  const llvm::DataLayout &Layout = Module->getDataLayout();
  if (Triple.getArch() != llvm::Triple::x86 || !Triple.isOSWindows() ||
      !Triple.isOSBinFormatCOFF() || Layout.getPointerSize(0) != 4)
    return reject("requires the PE32 Windows x86 ABI");
  if (Name.empty() || Module->getNamedValue(Name))
    return reject("callback name is empty or already owned");
  if (Blocks.empty() || Blocks.size() > limits::kMaxRegistrationEHStateWork)
    return reject("callback block set is empty or exceeds the work budget");
  std::set<llvm::BasicBlock *> Body;
  for (llvm::BasicBlock *Block : Blocks)
    if (!Block || Block->getParent() != &Parent ||
        Block == &Parent.getEntryBlock() || !Block->getTerminator() ||
        !Body.insert(Block).second)
      return reject("callback blocks do not have unique parent identities");
  if (!Body.count(&Entry))
    return reject("callback entry is outside its body");
  if (!Entry.phis().empty())
    return reject("callback entry PHIs have no proven runtime incoming value");

  std::set<llvm::BasicBlock *> Reached;
  std::vector<llvm::BasicBlock *> Work{&Entry};
  size_t WorkUsed = 0;
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

  std::set<llvm::AllocaInst *> PrivateSlots;
  if (Frame.StackPointerSlots.empty() != (Frame.StackBytes == 0) ||
      Frame.StackBytes > limits::kMaxRegistrationEHStateWork)
    return reject("private callback stack has no proven bounded size");
  for (llvm::AllocaInst *Slot : Frame.StackPointerSlots)
    if (!Slot || !isStaticEntryAlloca(*Slot, Parent) ||
        !Slot->getAllocatedType()->isIntegerTy(32) ||
        !llvm::cast<llvm::ConstantInt>(Slot->getArraySize())->isOne() ||
        !PrivateSlots.insert(Slot).second)
      return reject("private ESP slot is not a unique scalar entry alloca");

  const bool HasExceptionCell = Frame.ExceptionPointersFrame != nullptr;
  if (HasExceptionCell != Frame.ExceptionPointersOffset.has_value() ||
      (HasExceptionCell && Kind != X86RegistrationCallbackKind::Filter))
    return reject("exception-pointer cell is incomplete or not a filter cell");
  if (HasExceptionCell) {
    llvm::AllocaInst &Slot = *Frame.ExceptionPointersFrame;
    if (!isStaticEntryAlloca(Slot, Parent) || PrivateSlots.count(&Slot))
      return reject("exception-pointer frame is not a shared entry alloca");
    const auto Bytes = Slot.getAllocationSize(Layout);
    if (!Bytes || Bytes->isScalable() || Bytes->getFixedValue() < 4 ||
        Bytes->getFixedValue() > std::numeric_limits<uint32_t>::max() ||
        *Frame.ExceptionPointersOffset > Bytes->getFixedValue() - 4)
      return reject("exception-pointer cell lies outside the synthetic frame");
  }

  llvm::CallInst *ExistingEscape = nullptr;
  std::vector<llvm::AllocaInst *> Escaped;
  std::map<llvm::AllocaInst *, uint32_t> EscapeIndices;
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
          Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape) {
        if (ExistingEscape || &Block != &Parent.getEntryBlock())
          return reject("parent localescape is duplicated or outside entry");
        ExistingEscape = Call;
        for (llvm::Value *Operand : Call->args()) {
          auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(Operand);
          if (!Slot || !isStaticEntryAlloca(*Slot, Parent))
            return reject("existing localescape does not name a static alloca");
          EscapeIndices.try_emplace(Slot, Escaped.size());
          Escaped.push_back(Slot);
        }
      }

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
          llvm::isa<llvm::AllocaInst>(Instruction))
        return reject("callback contains an unsupported entry or control form");
      if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        HasReturn = true;
        if (Kind == X86RegistrationCallbackKind::Filter &&
            (!Return->getReturnValue() ||
             !Return->getReturnValue()->getType()->isIntegerTy(32)))
          return reject("filter does not return an exact i32 result");
      }
      for (llvm::Value *Operand : Instruction.operands())
        Inspect(Operand, 0);
    }
  if (!HasReturn || InvalidDependency)
    return reject("callback return or parent-value dependencies are unproven");
  if (HasExceptionCell)
    Needed.insert(Frame.ExceptionPointersFrame);
  for (llvm::Instruction &Instruction : Parent.getEntryBlock())
    if (auto *Slot = llvm::dyn_cast<llvm::AllocaInst>(&Instruction);
        Slot && Needed.count(Slot) && !EscapeIndices.count(Slot)) {
      EscapeIndices.emplace(Slot, Escaped.size());
      Escaped.push_back(Slot);
    }
  if (Escaped.size() + Arguments.size() > limits::kMaxRegistrationEHStateWork)
    return reject("escaped callback frame exceeds the work budget");

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
  llvm::Function *Recover = llvm::Intrinsic::getOrInsertDeclaration(
      Module, llvm::Intrinsic::localrecover);
  for (size_t Index = 0; Index < Escaped.size(); ++Index)
    if (llvm::AllocaInst *Slot = Escaped[Index]; Needed.count(Slot))
      Values[Slot] = Builder.CreateCall(
          Recover, {&Parent, ParentFrame, Builder.getInt32(Index)},
          Slot->getName() + ".recovered");

  llvm::Instruction *EscapeAt =
      ExistingEscape ? static_cast<llvm::Instruction *>(ExistingEscape)
                     : Parent.getEntryBlock().getTerminator();
  llvm::IRBuilder<> ParentBuilder(EscapeAt);
  for (llvm::Argument &Argument : Parent.args())
    if (Arguments.count(&Argument)) {
      auto *Slot = ParentBuilder.CreateAlloca(Argument.getType(), nullptr,
                                              "eh.argument");
      ParentBuilder.CreateStore(&Argument, Slot);
      const uint32_t Index = Escaped.size();
      Escaped.push_back(Slot);
      llvm::Value *Address = Builder.CreateCall(
          Recover, {&Parent, ParentFrame, Builder.getInt32(Index)},
          "argument.recovered");
      Values[&Argument] =
          Builder.CreateLoad(Argument.getType(), Address, Argument.getName());
    }
  if (!Escaped.empty()) {
    llvm::SmallVector<llvm::Value *, 8> Operands(Escaped.begin(),
                                                 Escaped.end());
    ParentBuilder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                 Module, llvm::Intrinsic::localescape),
                             Operands);
    if (ExistingEscape)
      ExistingEscape->eraseFromParent();
  }
  if (!PrivateSlots.empty()) {
    auto *Stack =
        Builder.CreateAlloca(llvm::ArrayType::get(I8Ty, Frame.StackBytes),
                             nullptr, "callback.stack");
    Stack->setAlignment(llvm::Align(16));
    llvm::Value *Top = Builder.CreateInBoundsGEP(
        I8Ty, Stack, Builder.getInt32(Frame.StackBytes), "callback.sp");
    llvm::Value *SP = Builder.CreatePtrToInt(Top, I32Ty);
    for (llvm::AllocaInst *Original : Frame.StackPointerSlots) {
      auto *Slot = Builder.CreateAlloca(I32Ty, nullptr, "callback.esp");
      Builder.CreateStore(SP, Slot);
      Values[Original] = Slot;
    }
  }
  if (HasExceptionCell) {
    // EH3/EH4's dispatcher passes EBP = registration + 16 to a filter. Its
    // EXCEPTION_POINTERS cell is one word before the registration, at EBP-20.
    llvm::Value *RuntimeCell = Builder.CreateGEP(
        I8Ty, RuntimeFrame, Builder.getInt32(-20), "runtime.exception.cell");
    llvm::Value *Pointers =
        Builder.CreateLoad(PointerTy, RuntimeCell, "exception.pointers");
    llvm::Value *LogicalCell = Builder.CreateInBoundsGEP(
        I8Ty, Values[Frame.ExceptionPointersFrame],
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

} // namespace neverd

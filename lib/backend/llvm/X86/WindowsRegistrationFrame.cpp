//===- WindowsRegistrationFrame.cpp - PE32 callback outlining -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include "WindowsRegistrationFramePrivate.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

namespace neverd {
namespace {
using x86_registration::CallbackPlan;
using x86_registration::filterResult;
using x86_registration::isStaticEntryAlloca;
using x86_registration::prepareCallback;
using x86_registration::reject;

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
  const auto &Frame = Plan.Frame;
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
    const uint64_t Offset = Kind == X86RegistrationRootKind::FramePointer
                                ? *Frame.EstablishedFramePointerOffset
                                : *Frame.StackPointerOffset;
    Clone->setMetadata(
        windows_eh_md::RegistrationRootAttachment,
        llvm::MDNode::get(
            Parent.getContext(),
            {llvm::ConstantAsMetadata::get(
                 llvm::ConstantInt::get(I8Ty, static_cast<uint8_t>(Kind))),
             llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                 llvm::Type::getInt64Ty(Parent.getContext()), Offset))}));
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
    if (auto *Return =
            llvm::dyn_cast<llvm::ReturnInst>(Clone->getTerminator())) {
      if (Kind == X86RegistrationCallbackKind::Finally ||
          !Return->getReturnValue()->getType()->isIntegerTy(32)) {
        llvm::IRBuilder<> B(Return);
        if (Kind == X86RegistrationCallbackKind::Finally)
          B.CreateRetVoid();
        else
          B.CreateRet(filterResult(*Return));
        Return->eraseFromParent();
      }
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

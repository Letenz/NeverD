//===- WindowsRegistrationFrameTests.cpp - x86 callback frame ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace neverd;

struct Fixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"registration-frame", Context};
  llvm::Function *Parent = nullptr;
  llvm::BasicBlock *Entry = nullptr;
  llvm::BasicBlock *Filter = nullptr;
  llvm::AllocaInst *Local = nullptr;

  Fixture() {
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                                {llvm::Type::getInt32Ty(Context)}, false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    llvm::IRBuilder<> B(Entry);
    Local = B.CreateAlloca(B.getInt32Ty(), nullptr, "local");
    B.CreateStore(Parent->getArg(0), Local);
    B.CreateRet(B.getInt32(0));
    Filter = llvm::BasicBlock::Create(Context, "filter", Parent);
    llvm::IRBuilder<> F(Filter);
    F.CreateRet(F.CreateLoad(F.getInt32Ty(), Local));
  }

  std::string text() {
    std::string Result;
    llvm::raw_string_ostream OS(Result);
    Module.print(OS, nullptr);
    return Result;
  }

  std::vector<llvm::CallInst *> calls(llvm::Function &Function,
                                      llvm::Intrinsic::ID ID) {
    std::vector<llvm::CallInst *> Result;
    for (llvm::BasicBlock &Block : Function)
      for (llvm::Instruction &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
            Call && Call->getIntrinsicID() == ID)
          Result.push_back(Call);
    return Result;
  }
};

TEST(WindowsRegistrationFrame, FilterRecoversTheParentsActualLocal) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function &Callback = **Result;
  EXPECT_TRUE(Callback.getReturnType()->isIntegerTy(32));
  EXPECT_EQ(Callback.arg_size(), 0u);
  auto EntryFrames = F.calls(Callback, llvm::Intrinsic::frameaddress);
  ASSERT_EQ(EntryFrames.size(), 1u);
  EXPECT_EQ(llvm::cast<llvm::ConstantInt>(EntryFrames[0]->getArgOperand(0))
                ->getZExtValue(),
            1u);
  auto RecoverFP = F.calls(Callback, llvm::Intrinsic::eh_recoverfp);
  ASSERT_EQ(RecoverFP.size(), 1u);
  EXPECT_EQ(RecoverFP[0]->getArgOperand(0), F.Parent);
  EXPECT_EQ(RecoverFP[0]->getArgOperand(1), EntryFrames[0]);
  auto Recover = F.calls(Callback, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(Recover[0]->getArgOperand(1), RecoverFP[0]);
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  ASSERT_EQ(Escape[0]->arg_size(), 1u);
  EXPECT_EQ(Escape[0]->getArgOperand(0), F.Local);
  std::string Errors;
  llvm::raw_string_ostream OS(Errors);
  EXPECT_FALSE(llvm::verifyModule(F.Module, &OS)) << Errors;
}

TEST(WindowsRegistrationFrame, FinallyUsesItsParentFrameArgument) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Finally,
      "finally.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function &Callback = **Result;
  EXPECT_TRUE(Callback.getReturnType()->isVoidTy());
  ASSERT_EQ(Callback.arg_size(), 2u);
  EXPECT_TRUE(Callback.getArg(0)->getType()->isIntegerTy(8));
  EXPECT_TRUE(Callback.getArg(0)->hasAttribute(llvm::Attribute::ZExt));
  EXPECT_TRUE(F.calls(Callback, llvm::Intrinsic::frameaddress).empty());
  EXPECT_TRUE(F.calls(Callback, llvm::Intrinsic::eh_recoverfp).empty());
  auto Recover = F.calls(Callback, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(Recover[0]->getArgOperand(1), Callback.getArg(1));
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, PreservesExistingEscapeIndices) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Prior = B.CreateAlloca(B.getInt32Ty(), nullptr, "prior");
  B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                   &F.Module, llvm::Intrinsic::localescape),
               {Prior});
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  ASSERT_EQ(Escape[0]->arg_size(), 2u);
  EXPECT_EQ(Escape[0]->getArgOperand(0), Prior);
  EXPECT_EQ(Escape[0]->getArgOperand(1), F.Local);
  auto Recover = F.calls(**Result, llvm::Intrinsic::localrecover);
  ASSERT_EQ(Recover.size(), 1u);
  EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Recover[0]->getArgOperand(2))
                ->getZExtValue(),
            1u);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, ParentArgumentsAreRecoveredFromEscapedStorage) {
  Fixture F;
  F.Filter->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> B(F.Filter);
  B.CreateRet(
      B.CreateAdd(B.CreateLoad(B.getInt32Ty(), F.Local), F.Parent->getArg(0)));
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(F.calls(**Result, llvm::Intrinsic::localrecover).size(), 2u);
  auto Escape = F.calls(*F.Parent, llvm::Intrinsic::localescape);
  ASSERT_EQ(Escape.size(), 1u);
  EXPECT_EQ(Escape[0]->arg_size(), 2u);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame,
     CopiesExceptionPointersIntoTheBoundedSourceCell) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr, "frame");
  X86RegistrationCallbackFrame Spec;
  Spec.ExceptionPointersFrame = Frame;
  Spec.ExceptionPointersOffset = 40;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  bool HasRuntimeOffset = false;
  bool HasSourceOffset = false;
  bool HasPointerStore = false;
  for (llvm::Instruction &Instruction : (*Result)->getEntryBlock()) {
    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&Instruction))
      if (auto *Offset =
              llvm::dyn_cast<llvm::ConstantInt>(GEP->getOperand(1))) {
        HasRuntimeOffset |= Offset->getSExtValue() == -20;
        HasSourceOffset |= Offset->getZExtValue() == 40;
      }
    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
      HasPointerStore |= Store->getValueOperand()->getType()->isPointerTy();
  }
  EXPECT_TRUE(HasRuntimeOffset);
  EXPECT_TRUE(HasSourceOffset);
  EXPECT_TRUE(HasPointerStore);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, CallbackStackDoesNotOverwriteTheParentESPSlot) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *ESP = B.CreateAlloca(B.getInt32Ty(), nullptr, "esp.root");
  F.Filter->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> FB(F.Filter);
  FB.CreateStore(FB.getInt32(17), ESP);
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), ESP));
  X86RegistrationCallbackFrame Spec;
  Spec.StackPointerSlots = llvm::ArrayRef(ESP);
  Spec.StackBytes = 128;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  for (llvm::CallInst *Escape :
       F.calls(*F.Parent, llvm::Intrinsic::localescape))
    for (llvm::Value *Operand : Escape->args())
      EXPECT_NE(Operand, ESP);
  for (llvm::BasicBlock &Block : **Result)
    for (llvm::Instruction &Instruction : Block)
      for (llvm::Value *Operand : Instruction.operands())
        EXPECT_NE(Operand, ESP);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame,
     RebuildsEntryFrameAddressesFromRecoveredAllocas) {
  Fixture F;
  llvm::IRBuilder<> B(F.Entry->getTerminator());
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64), nullptr, "frame");
  auto *FrameEnd = B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(48));
  auto *FrameInt = B.CreatePtrToInt(FrameEnd, B.getInt64Ty());
  auto *FrameLow = B.CreateTrunc(FrameInt, B.getInt32Ty());
  F.Filter->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> FB(F.Filter);
  auto *Cell =
      FB.CreateIntToPtr(FB.CreateSub(FrameLow, FB.getInt32(4)), FB.getPtrTy());
  FB.CreateRet(FB.CreateLoad(FB.getInt32Ty(), Cell));
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  for (llvm::BasicBlock &Block : **Result)
    for (llvm::Instruction &Instruction : Block)
      for (llvm::Value *Operand : Instruction.operands())
        if (const auto *Definition = llvm::dyn_cast<llvm::Instruction>(Operand))
          EXPECT_EQ(Definition->getFunction(), *Result);
  EXPECT_FALSE(llvm::verifyModule(F.Module));
}

TEST(WindowsRegistrationFrame, CodegenEmitsThePE32FilterTableAndFrameRecovery) {
  Fixture F;
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  llvm::Function *Callback = *Result;
  auto *Protected = llvm::BasicBlock::Create(F.Context, "protected", F.Parent);
  auto *Normal = llvm::BasicBlock::Create(F.Context, "normal", F.Parent);
  auto *Dispatch = llvm::BasicBlock::Create(F.Context, "dispatch", F.Parent);
  auto *Pad = llvm::BasicBlock::Create(F.Context, "pad", F.Parent);
  auto *Handler = llvm::BasicBlock::Create(F.Context, "handler", F.Parent);
  auto *PersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(F.Context), {}, true);
  F.Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      F.Module.getOrInsertFunction("_except_handler3", PersonalityTy)
          .getCallee()));
  F.Module.addModuleFlag(llvm::Module::Warning, "eh-asynch", 1);
  F.Entry->getTerminator()->eraseFromParent();
  llvm::IRBuilder<> Entry(F.Entry);
  Entry.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                         &F.Module, llvm::Intrinsic::seh_try_begin),
                     Protected, Dispatch);
  llvm::IRBuilder<> Body(Protected);
  auto *Value = Body.CreateLoad(Body.getInt32Ty(), F.Local);
  Value->setVolatile(true);
  Body.CreateInvoke(llvm::Intrinsic::getOrInsertDeclaration(
                        &F.Module, llvm::Intrinsic::seh_try_end),
                    Normal, Dispatch);
  llvm::IRBuilder<>(Normal).CreateRet(Value);
  llvm::IRBuilder<> DB(Dispatch);
  auto *Switch =
      DB.CreateCatchSwitch(llvm::ConstantTokenNone::get(F.Context), nullptr, 1);
  Switch->addHandler(Pad);
  llvm::IRBuilder<> PB(Pad);
  auto *Catch = PB.CreateCatchPad(Switch, {Callback});
  PB.CreateCatchRet(Catch, Handler);
  llvm::IRBuilder<> HB(Handler);
  HB.CreateRet(HB.getInt32(42));
  std::string Errors;
  llvm::raw_string_ostream ErrorStream(Errors);
  ASSERT_FALSE(llvm::verifyModule(F.Module, &ErrorStream)) << Errors;
  static std::once_flag Once;
  std::call_once(Once, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
  });
  const llvm::Target *Target =
      llvm::TargetRegistry::lookupTarget(F.Module.getTargetTriple(), Errors);
  ASSERT_NE(Target, nullptr) << Errors;
  llvm::TargetOptions Options;
  std::unique_ptr<llvm::TargetMachine> Machine(Target->createTargetMachine(
      F.Module.getTargetTriple(), "i686", "", Options, llvm::Reloc::Static));
  ASSERT_NE(Machine, nullptr);
  F.Module.setDataLayout(Machine->createDataLayout());
  llvm::SmallVector<char, 0> Bytes;
  llvm::raw_svector_ostream OS(Bytes);
  llvm::legacy::PassManager Passes;
  ASSERT_FALSE(Machine->addPassesToEmitFile(Passes, OS, nullptr,
                                            llvm::CodeGenFileType::ObjectFile));
  Passes.run(F.Module);
  auto Object =
      llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
          llvm::StringRef(Bytes.data(), Bytes.size()), "registration.obj"));
  ASSERT_TRUE(static_cast<bool>(Object)) << llvm::toString(Object.takeError());
  auto *COFF = llvm::dyn_cast<llvm::object::COFFObjectFile>(Object->get());
  ASSERT_NE(COFF, nullptr);
  EXPECT_EQ(COFF->getMachine(), llvm::COFF::IMAGE_FILE_MACHINE_I386);
  bool HasScope = false;
  for (llvm::object::SectionRef Section : COFF->sections()) {
    auto Name = Section.getName();
    ASSERT_TRUE(static_cast<bool>(Name)) << llvm::toString(Name.takeError());
    if (*Name != ".xdata")
      continue;
    auto Contents = Section.getContents();
    ASSERT_TRUE(static_cast<bool>(Contents))
        << llvm::toString(Contents.takeError());
    EXPECT_EQ(Contents->size(), 12u);
    unsigned AbsoluteReferences = 0;
    for (llvm::object::RelocationRef Relocation : Section.relocations())
      AbsoluteReferences +=
          Relocation.getType() == llvm::COFF::IMAGE_REL_I386_DIR32;
    EXPECT_EQ(AbsoluteReferences, 2u);
    HasScope = true;
  }
  EXPECT_TRUE(HasScope);
}

TEST(WindowsRegistrationFrame, RejectsNonlocalValuesBeforeMutatingTheModule) {
  Fixture F;
  llvm::IRBuilder<> Entry(F.Entry->getTerminator());
  auto *Value = Entry.CreateLoad(Entry.getInt32Ty(), F.Local, "parent.value");
  F.Filter->getTerminator()->eraseFromParent();
  llvm::IRBuilder<>(F.Filter).CreateRet(Value);
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("dependencies"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsOrdinaryFlowIntoRuntimeCallback) {
  Fixture F;
  F.Entry->getTerminator()->eraseFromParent();
  llvm::IRBuilder<>(F.Entry).CreateBr(F.Filter);
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined");
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("ordinary parent flow"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

TEST(WindowsRegistrationFrame, RejectsAnOutOfBoundsExceptionCellAtomically) {
  Fixture F;
  X86RegistrationCallbackFrame Spec;
  Spec.ExceptionPointersFrame = F.Local;
  Spec.ExceptionPointersOffset = 1;
  const std::string Before = F.text();
  auto Result = outlineX86RegistrationCallback(
      *F.Parent, *F.Filter, {F.Filter}, X86RegistrationCallbackKind::Filter,
      "filter.outlined", Spec);
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_NE(llvm::toString(Result.takeError()).find("outside"),
            std::string::npos);
  EXPECT_EQ(F.text(), Before);
}

} // namespace

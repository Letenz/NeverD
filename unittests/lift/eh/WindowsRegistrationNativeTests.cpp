//===- WindowsRegistrationNativeTests.cpp - PE32 SEH lowering tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/object/PELayout.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>

namespace neverd {
class MedLLVMEmitterTestPeer {
public:
  static void prepare(MedLLVMEmitter &E, llvm::Module &M, llvm::Function &F,
                      const MedFunc &Source, llvm::AllocaInst &Frame) {
    E.Ctx = &M.getContext();
    E.Mod = &M;
    E.TargetArch = Arch::X86;
    E.TargetFormat = BinaryFormat::COFF;
    E.CurFunc = &F;
    E.CurMedFunc = &Source;
    E.FrameAlloca = &Frame;
    E.FrameEntrySPOffset = 64;
  }
  static void chain(MedLLVMEmitter &E, llvm::Instruction &I, va_t Address,
                    int Seq) {
    E.RegistrationChainIR.emplace(std::make_pair(Address, Seq), &I);
  }
  static void call(MedLLVMEmitter &E, llvm::CallInst &I, va_t Address) {
    E.CallSiteAddrs.emplace(&I, Address);
    I.setMetadata(language_eh_md::InternalSourceCallAttachment,
                  llvm::MDNode::get(
                      I.getContext(),
                      {llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                          llvm::Type::getInt64Ty(I.getContext()), Address))}));
  }
  static void forgetCall(MedLLVMEmitter &E, llvm::CallInst &I) {
    E.CallSiteAddrs.erase(&I);
  }
  static bool lower(MedLLVMEmitter &E, const MedFunc &Source, llvm::Function &F,
                    const std::map<int, llvm::BasicBlock *> &Blocks) {
    return E.emitNativeX86RegistrationSEH(Source, F, Blocks);
  }
};
} // namespace neverd

namespace {
using namespace neverd;

TEST(WindowsRegistrationNative, InputPE32PreservesItsCheckedSourceContract) {
  const char *Path = std::getenv("NEVERD_REGISTRATION_INPUT_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_INPUT_PE32 to the runtime fixture";
  auto Loaded = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  auto &Image = *Loaded;
  for (const auto &Diagnostic : Image.ExceptionMetadata.Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_EQ(Image.ExceptionMetadata.Functions.size(), 1u);
  const auto &EH = Image.ExceptionMetadata.Functions.front();
  for (const auto &Diagnostic : EH.Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  ASSERT_TRUE(EH.Registration);
  ASSERT_EQ(EH.ParseStatus, ExceptionParseStatus::Complete);
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image));
  auto Low = CFGBuilder().build(Image, Decoder, EH.CodeRange.Begin, "guarded");
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_TRUE(Low.RegistrationStates->RegistrationLifetimeComplete);
  EXPECT_TRUE(Low.RegistrationStates->ChainOperationsComplete);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  if (!Low.RegistrationStates->Complete)
    for (const auto &State : Low.RegistrationStates->Blocks) {
      llvm::errs() << "block " << State.BlockId << " @ "
                   << llvm::format_hex(State.Range.Begin, 10)
                   << " unknown=" << State.Unknown
                   << " callback=" << State.CallbackOnly << " levels=";
      for (auto Level : State.Levels)
        llvm::errs() << Level << ',';
      llvm::errs() << '\n';
    }
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  auto Module = Emitter.emit({Med}, Context, "input-registration", Arch::X86,
                             {}, &Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  auto *Parent = Module->getFunction("guarded");
  ASSERT_TRUE(Parent);
  if (!Parent->getMetadata(windows_eh_md::NativeAttachment))
    Module->print(llvm::errs(), nullptr);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  if (const char *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Module->print(Stream, nullptr);
  }
#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
  auto Error = validateCOFFRegistrationIR(*Parent, EH, Image);
  if (Error)
    Module->print(llvm::errs(), nullptr);
  ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  auto RejectMutation = [&](llvm::StringRef Label, auto Mutate,
                            llvm::StringRef Diagnostic) {
    SCOPED_TRACE(Label.str());
    auto Altered = llvm::CloneModule(*Module);
    auto *Function = Altered->getFunction(Parent->getName());
    ASSERT_TRUE(Function);
    ASSERT_TRUE(Mutate(*Function));
    ASSERT_FALSE(llvm::verifyModule(*Altered, &llvm::errs()));
    auto Failure = validateCOFFRegistrationIR(*Function, EH, Image);
    ASSERT_TRUE(bool(Failure));
    const auto Message = llvm::toString(std::move(Failure));
    EXPECT_NE(Message.find(Diagnostic.str()), std::string::npos) << Message;
  };
  if (EH.Personality == ExceptionPersonality::ExceptHandler4) {
    ASSERT_TRUE(Low.RegistrationStates->SecurityCookiesComplete);
    EXPECT_EQ(Low.RegistrationStates->SecurityCookieVA,
              Image.Base + Image.DynInfo.SecurityCookieRVA);
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation)
      RejectMutation(
          "compiler cookie retains the exact external image storage",
          [Mutation](llvm::Function &F) {
            auto *Cookie = F.getParent()->getNamedGlobal("__security_cookie");
            if (!Cookie)
              return false;
            if (Mutation == 0)
              Cookie->setInitializer(llvm::ConstantInt::get(
                  llvm::Type::getInt32Ty(F.getContext()), 42));
            if (Mutation == 1)
              Cookie->setConstant(true);
            if (Mutation == 2)
              Cookie->setDLLStorageClass(
                  llvm::GlobalValue::DLLImportStorageClass);
            return true;
          },
          "cookie frame, image storage or CRT wrapper");
    for (bool Table : {false, true})
      RejectMutation(
          "edited LLVM cannot overwrite the EH4 runtime contract",
          [&](llvm::Function &F) {
            auto *Cookie = F.getParent()->getNamedGlobal("__security_cookie");
            if (!Cookie)
              return false;
            llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
            llvm::Value *Target = Cookie;
            if (Table)
              Target = new llvm::GlobalVariable(
                  *F.getParent(), B.getInt32Ty(), false,
                  llvm::GlobalValue::ExternalLinkage, nullptr,
                  "__nd_data_" +
                      llvm::utohexstr(EH.Registration->ScopeTableVA));
            B.CreateStore(B.getInt32(42), Target)->setVolatile(true);
            return true;
          },
          "EH4 cookie or scope table is written");
    if (EH.Registration->GSCookieOffset != -2) {
      for (unsigned Mutation = 0; Mutation != 4; ++Mutation)
        RejectMutation(
            "compiler GS checker keeps its ABI and authenticated identity",
            [Mutation](llvm::Function &F) {
              auto *Check =
                  F.getParent()->getFunction("__security_check_cookie");
              if (!Check)
                return false;
              if (Mutation == 0)
                Check->setCallingConv(llvm::CallingConv::C);
              if (Mutation == 1)
                Check->removeParamAttr(0, llvm::Attribute::InReg);
              if (Mutation == 2) {
                Check->setDSOLocal(false);
                Check->setDLLStorageClass(
                    llvm::GlobalValue::DLLImportStorageClass);
              }
              if (Mutation == 3)
                rewrite_source::setOriginalVA(*Check, 0x401000);
              return true;
            },
            "EH4 GS cookie checker");
    }
  }
  RejectMutation(
      "unmarked image memory read cannot bypass source occurrence checks",
      [](llvm::Function &F) {
        llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
        auto *Global =
            new llvm::GlobalVariable(*F.getParent(), B.getInt32Ty(), false,
                                     llvm::GlobalValue::ExternalLinkage,
                                     nullptr, "__nd_data_403000.unproved-read");
        B.CreateLoad(B.getInt32Ty(), Global)->setVolatile(true);
        return true;
      },
      "unproved LLVM escape or access");
  for (bool Atomic : {false, true})
    RejectMutation(
        "unindexed image memory operation cannot launder a caller address",
        [Atomic](llvm::Function &F) {
          llvm::IRBuilder<> B(F.getEntryBlock().getTerminator());
          auto *Global = new llvm::GlobalVariable(
              *F.getParent(), B.getInt32Ty(), false,
              llvm::GlobalValue::ExternalLinkage, nullptr,
              "__nd_data_403000.unproved-memory");
          if (Atomic)
            B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, Global, B.getInt32(0),
                              llvm::Align(4),
                              llvm::AtomicOrdering::SequentiallyConsistent);
          else
            B.CreateMemCpy(Global, llvm::Align(1), Global, llvm::Align(1), 4);
          return true;
        },
        "unproved LLVM escape or access");
  RejectMutation(
      "changed parent stack cleanup",
      [](llvm::Function &F) {
        F.setCallingConv(llvm::CallingConv::X86_StdCall);
        return true;
      },
      "stack cleanup ABI");
  bool HasNormalFinally = false;
  for (const auto &Block : *Parent)
    for (const auto &I : Block)
      HasNormalFinally |=
          I.getMetadata(windows_eh_md::RegistrationFinallyCallAttachment) !=
          nullptr;
  if (HasNormalFinally)
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation)
      RejectMutation(
          "changed ordinary finally entry protocol",
          [Mutation](llvm::Function &F) {
            for (auto &Block : F)
              for (auto &I : Block)
                if (I.getMetadata(
                        windows_eh_md::RegistrationFinallyCallAttachment)) {
                  auto *Call = llvm::cast<llvm::CallBase>(&I);
                  if (Mutation == 0)
                    I.setMetadata(
                        windows_eh_md::RegistrationFinallyCallAttachment,
                        nullptr);
                  else if (Mutation == 1)
                    Call->setArgOperand(
                        0, llvm::ConstantInt::get(
                               llvm::Type::getInt8Ty(F.getContext()), 1));
                  else
                    Call->setArgOperand(
                        1, llvm::ConstantPointerNull::get(
                               llvm::PointerType::get(F.getContext(), 0)));
                  return true;
                }
            return false;
          },
          Mutation == 0 ? "no recovered callback" : "normal parent-frame ABI");
  bool HasIncomingFrame = false;
  for (const auto &F : *Module)
    for (const auto &Block : F)
      for (const auto &I : Block)
        HasIncomingFrame |=
            I.getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment) !=
            nullptr;
  if (HasIncomingFrame)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation)
      RejectMutation(
          "changed incoming caller stack projection",
          [Mutation](llvm::Function &F) {
            for (auto &Function : *F.getParent())
              for (auto &Block : Function)
                for (auto &I : Block)
                  if (I.getMetadata(
                          windows_eh_md::RegistrationIncomingFrameAttachment)) {
                    if (Mutation == 0)
                      I.setMetadata(
                          windows_eh_md::RegistrationIncomingFrameAttachment,
                          nullptr);
                    else {
                      auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I);
                      auto *Pointer = Load ? Load->getPointerOperand()
                                           : llvm::cast<llvm::StoreInst>(&I)
                                                 ->getPointerOperand();
                      auto *GEP = llvm::cast<llvm::GetElementPtrInst>(Pointer);
                      if (Mutation == 4) {
                        if (!Load)
                          continue;
                        auto *Base = llvm::cast<llvm::Instruction>(
                            GEP->getPointerOperand());
                        auto *Term = Function.getEntryBlock().getTerminator();
                        Base->moveBefore(Term->getIterator());
                        GEP->moveBefore(Term->getIterator());
                        I.moveBefore(Term->getIterator());
                      } else if (Mutation == 3) {
                        if (Load)
                          Load->setAlignment(llvm::Align(16));
                        else
                          llvm::cast<llvm::StoreInst>(&I)->setAlignment(
                              llvm::Align(16));
                      } else if (Mutation == 1)
                        GEP->setOperand(
                            1, llvm::ConstantInt::get(
                                   llvm::Type::getInt32Ty(F.getContext()), 12));
                      else
                        GEP->setIsInBounds(true);
                    }
                    return true;
                  }
            return false;
          },
          Mutation == 0   ? "access set is incomplete"
          : Mutation == 3 ? "source memory"
          : Mutation == 4 ? "execution segment"
                          : "physical stack projection");
  if (HasIncomingFrame)
    RejectMutation(
        "swapped incoming recipes keep their original execution events",
        [](llvm::Function &F) {
          std::vector<llvm::Instruction *> Loads;
          for (auto &Function : *F.getParent())
            for (auto &Block : Function)
              for (auto &I : Block)
                if (llvm::isa<llvm::LoadInst>(I) &&
                    I.getMetadata(
                        windows_eh_md::RegistrationIncomingFrameAttachment))
                  Loads.push_back(&I);
          if (Loads.size() < 2)
            return false;
          auto *A = Loads[0]->getMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment);
          auto *B = Loads[1]->getMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment);
          Loads[0]->setMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment, B);
          Loads[1]->setMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment, A);
          auto *PA = llvm::cast<llvm::GetElementPtrInst>(
              llvm::cast<llvm::LoadInst>(Loads[0])->getPointerOperand());
          auto *PB = llvm::cast<llvm::GetElementPtrInst>(
              llvm::cast<llvm::LoadInst>(Loads[1])->getPointerOperand());
          auto *Offset = PA->getOperand(1);
          PA->setOperand(1, PB->getOperand(1));
          PB->setOperand(1, Offset);
          return true;
        },
        "source execution occurrence");
  RejectMutation(
      "changed preserved callee stack cleanup",
      [](llvm::Function &F) {
        for (auto &Block : F)
          for (auto &I : Block)
            if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
              if (auto *Callee = Call->getCalledFunction();
                  Callee && Callee->isDeclaration() && !Callee->isIntrinsic()) {
                Callee->setCallingConv(llvm::CallingConv::X86_StdCall);
                return true;
              }
        return false;
      },
      "stack cleanup ABI");
  if (EH.Registration->Scopes.size() > 1 &&
      !EH.Registration->Scopes.back().IsFinally)
    RejectMutation(
        "nested handler bypasses outer scope entry",
        [](llvm::Function &F) {
          for (auto &Block : F)
            if (auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(
                    Block.getTerminator()))
              if (auto *Enter = llvm::dyn_cast<llvm::InvokeInst>(
                      Return->getSuccessor()->getTerminator());
                  Enter && Enter->getCalledFunction() &&
                  Enter->getCalledFunction()->getIntrinsicID() ==
                      llvm::Intrinsic::seh_scope_begin) {
                Return->setSuccessor(Enter->getNormalDest());
                return true;
              }
          return false;
        },
        "scope entry");
  RejectMutation(
      "missing logical frame identity",
      [](llvm::Function &F) {
        for (auto &B : F)
          for (auto &I : B)
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment)) {
              I.setMetadata(windows_eh_md::RegistrationFrameAttachment,
                            nullptr);
              return true;
            }
        return false;
      },
      "compiler-owned identity");
  RejectMutation(
      "bypass asynchronous scope entry",
      [](llvm::Function &F) {
        for (auto &B : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(B.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            for (auto &Predecessor : F) {
              auto *Terminator = Predecessor.getTerminator();
              for (unsigned I = 0; I < Terminator->getNumSuccessors(); ++I)
                if (Terminator->getSuccessor(I) == &B) {
                  Terminator->setSuccessor(I, Invoke->getNormalDest());
                  return true;
                }
            }
          }
        return false;
      },
      "coff registration patch:");
  for (bool Write : {false, true})
    RejectMutation(
        Write ? "ordinary store before logical frame"
              : "ordinary load after logical frame",
        [Write](llvm::Function &F) {
          llvm::AllocaInst *Frame = nullptr;
          for (auto &B : F)
            for (auto &I : B)
              if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
                Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (!Frame)
            return false;
          for (auto &Block : F)
            if (auto *Invoke =
                    llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
                Invoke && Invoke->getCalledFunction() &&
                Invoke->getCalledFunction()->getIntrinsicID() ==
                    llvm::Intrinsic::seh_scope_begin) {
              llvm::IRBuilder<> B(
                  &*Invoke->getNormalDest()->getFirstInsertionPt());
              auto Bytes =
                  Frame->getAllocationSize(F.getParent()->getDataLayout());
              if (!Bytes || Bytes->isScalable())
                return false;
              auto *Address = B.CreateGEP(
                  B.getInt8Ty(), Frame,
                  B.getInt32(Write ? uint32_t(-4) : Bytes->getFixedValue()));
              if (Write)
                B.CreateStore(B.getInt32(0), Address);
              else
                B.CreateLoad(B.getInt32Ty(), Address);
              return true;
            }
          return false;
        },
        "unproved LLVM escape or access");
  RejectMutation(
      "truncated frame pointer laundering",
      [](llvm::Function &F) {
        llvm::AllocaInst *Frame = nullptr;
        for (auto &Block : F)
          for (auto &I : Block)
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
              Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!Frame)
          return false;
        for (auto &Block : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            llvm::IRBuilder<> B(
                &*Invoke->getNormalDest()->getFirstInsertionPt());
            auto *Address = B.CreatePtrToInt(Frame, B.getInt32Ty());
            auto *Low = B.CreateTrunc(Address, B.getInt8Ty());
            auto *Wide = B.CreateZExt(Low, B.getInt32Ty());
            auto *Pointer = B.CreateIntToPtr(Wide, B.getPtrTy());
            B.CreateStore(Address, Pointer);
            return true;
          }
        return false;
      },
      "unproved LLVM escape or access");
  RejectMutation(
      "compiler registration head escape",
      [](llvm::Function &F) {
        for (auto &Block : F)
          for (auto &I : Block) {
            auto *Head = llvm::dyn_cast<llvm::LoadInst>(&I);
            if (!Head || Head->getPointerAddressSpace() != 257)
              continue;
            auto *Pointer = Head->getNextNode();
            auto *Previous = Pointer ? Pointer->getNextNode() : nullptr;
            if (!Previous || !Previous->getNextNode())
              return false;
            llvm::IRBuilder<> B(Previous->getNextNode());
            auto *Global = new llvm::GlobalVariable(
                *F.getParent(), B.getInt32Ty(), false,
                llvm::GlobalValue::ExternalLinkage, nullptr, "leaked.head");
            B.CreateStore(Head, Global);
            return true;
          }
        return false;
      },
      "source previous head");
  RejectMutation(
      "inline assembly registration clobber",
      [](llvm::Function &F) {
        for (auto &Block : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            llvm::IRBuilder<> B(
                &*Invoke->getNormalDest()->getFirstInsertionPt());
            auto *Type = llvm::FunctionType::get(B.getVoidTy(), false);
            auto *Assembly = llvm::InlineAsm::get(Type, "movl $$0, %fs:0",
                                                  "~{memory}", true);
            B.CreateCall(Type, Assembly);
            return true;
          }
        return false;
      },
      "unproved LLVM escape or access");
  for (bool ScopeEnd : {false, true})
    RejectMutation(
        ScopeEnd ? "unanchored scope end" : "unanchored plain call",
        [ScopeEnd](llvm::Function &F) {
          for (auto &Block : F)
            if (auto *Invoke =
                    llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
                Invoke && Invoke->getCalledFunction() &&
                Invoke->getCalledFunction()->getIntrinsicID() ==
                    llvm::Intrinsic::seh_scope_begin) {
              llvm::IRBuilder<> B(
                  &*Invoke->getNormalDest()->getFirstInsertionPt());
              if (ScopeEnd)
                B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                    F.getParent(), llvm::Intrinsic::seh_scope_end));
              else
                B.CreateCall(F.getParent()->getOrInsertFunction(
                    "unknown_state_call", B.getVoidTy()));
              return true;
            }
          return false;
        },
        ScopeEnd ? "unanchored SEH scope state change" : "unanchored call");
  RejectMutation(
      "missing callback runtime root identity",
      [](llvm::Function &F) {
        for (auto &Helper : *F.getParent())
          for (auto &Block : Helper)
            for (auto &I : Block)
              if (I.getMetadata(windows_eh_md::RegistrationRootAttachment)) {
                I.setMetadata(windows_eh_md::RegistrationRootAttachment,
                              nullptr);
                return true;
              }
        return false;
      },
      "runtime root set");
  for (bool Redirect : {false, true})
    RejectMutation(
        Redirect ? "redirected callback root" : "late callback root",
        [Redirect](llvm::Function &F) {
          for (auto &Helper : *F.getParent())
            for (auto &Block : Helper)
              for (auto &I : Block) {
                auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
                if (!Store || !Store->getMetadata(
                                  windows_eh_md::RegistrationRootAttachment))
                  continue;
                if (Redirect) {
                  llvm::IRBuilder<> B(
                      &*Helper.getEntryBlock().getFirstInsertionPt());
                  Store->setOperand(1, B.CreateAlloca(B.getInt32Ty()));
                  return true;
                }
                for (auto *User : Store->getPointerOperand()->users())
                  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(User)) {
                    Store->moveAfter(Load);
                    return true;
                  }
              }
          return false;
        },
        Redirect ? "unused storage" : "dominate");
  RejectMutation(
      "logical exception cell read before runtime bridge",
      [](llvm::Function &F) {
        for (auto &Helper : *F.getParent())
          for (auto &Block : Helper)
            for (auto &I : Block) {
              auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I);
              auto *Load =
                  Store
                      ? llvm::dyn_cast<llvm::LoadInst>(Store->getValueOperand())
                      : nullptr;
              auto *Cell = Load ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                      Load->getPointerOperand())
                                : nullptr;
              auto *Runtime = Cell ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                         Cell->getPointerOperand())
                                   : nullptr;
              if (!Runtime ||
                  Runtime->getIntrinsicID() != llvm::Intrinsic::frameaddress)
                continue;
              llvm::IRBuilder<> B(Store);
              B.CreateLoad(B.getPtrTy(), Store->getPointerOperand());
              return true;
            }
        return false;
      },
      "unproved LLVM escape or access");
  RejectMutation(
      "callback spill overwritten on a self-loop backedge",
      [](llvm::Function &F) {
        llvm::AllocaInst *LogicalFrame = nullptr;
        llvm::IntrinsicInst *Escape = nullptr;
        for (auto &Block : F)
          for (auto &I : Block) {
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
              LogicalFrame = llvm::dyn_cast<llvm::AllocaInst>(&I);
            if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
                Call && Call->getIntrinsicID() == llvm::Intrinsic::localescape)
              Escape = Call;
          }
        if (!LogicalFrame || !Escape)
          return false;
        for (auto &Helper : *F.getParent()) {
          if (&Helper == &F || Helper.isDeclaration() || Helper.arg_size())
            continue;
          llvm::IntrinsicInst *Recover = nullptr;
          for (auto &I : Helper.getEntryBlock())
            if (auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
                Call &&
                Call->getIntrinsicID() == llvm::Intrinsic::localrecover &&
                llvm::isa<llvm::ConstantInt>(Call->getArgOperand(2)) &&
                llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))
                        ->getZExtValue() < Escape->arg_size() &&
                Escape->getArgOperand(
                    llvm::cast<llvm::ConstantInt>(Call->getArgOperand(2))
                        ->getZExtValue()) == LogicalFrame)
              Recover = Call;
          if (!Recover)
            continue;
          auto &Context = F.getContext();
          llvm::IRBuilder<> Setup(Helper.getEntryBlock().getTerminator());
          auto *Spill = Setup.CreateAlloca(Setup.getInt32Ty());
          auto *Frame32 = Setup.CreatePtrToInt(Recover, Setup.getInt32Ty());
          Setup.CreateStore(Frame32, Spill);
          auto *Loop =
              llvm::BasicBlock::Create(Context, "mutated.loop", &Helper);
          for (auto &Block : Helper)
            if (auto *Return =
                    llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator())) {
              auto *Done = Block.splitBasicBlock(Return, "mutated.done");
              llvm::cast<llvm::UncondBrInst>(Block.getTerminator())
                  ->setSuccessor(0, Loop);
              llvm::IRBuilder<> B(Loop);
              auto *Address = B.CreateLoad(B.getInt32Ty(), Spill);
              B.CreateStore(Frame32, B.CreateIntToPtr(Address, B.getPtrTy()));
              B.CreateStore(B.getInt32(0), Spill);
              auto *Again = new llvm::GlobalVariable(
                  *F.getParent(), B.getInt1Ty(), false,
                  llvm::GlobalValue::ExternalLinkage, nullptr, "repeat");
              B.CreateCondBr(B.CreateLoad(B.getInt1Ty(), Again), Loop, Done);
              return true;
            }
        }
        return false;
      },
      "unproved LLVM escape or access");
  for (bool WideGEP : {false, true})
    RejectMutation(
        WideGEP ? "inbounds wide-index overflow" : "nonnegative frame cast",
        [WideGEP](llvm::Function &F) {
          llvm::AllocaInst *Frame = nullptr;
          for (auto &Block : F)
            for (auto &I : Block)
              if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
                Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
          if (!Frame)
            return false;
          for (auto &Block : F)
            if (auto *Invoke =
                    llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
                Invoke && Invoke->getCalledFunction() &&
                Invoke->getCalledFunction()->getIntrinsicID() ==
                    llvm::Intrinsic::seh_scope_begin) {
              llvm::IRBuilder<> B(
                  &*Invoke->getNormalDest()->getFirstInsertionPt());
              llvm::Value *Address;
              if (WideGEP)
                Address = B.CreateInBoundsGEP(B.getInt8Ty(), Frame,
                                              B.getInt64(uint64_t(1) << 32));
              else {
                auto *Integer = B.CreatePtrToInt(Frame, B.getInt32Ty());
                auto *Wide = llvm::cast<llvm::Instruction>(
                    B.CreateZExt(Integer, B.getInt64Ty()));
                Wide->setNonNeg();
                Address = B.CreateIntToPtr(Wide, B.getPtrTy());
              }
              B.CreateLoad(B.getInt32Ty(), Address);
              return true;
            }
          return false;
        },
        "unproved LLVM escape or access");
  RejectMutation(
      "poison nusw intermediate canceled by integer wrap",
      [](llvm::Function &F) {
        llvm::AllocaInst *Frame = nullptr;
        for (auto &Block : F)
          for (auto &I : Block)
            if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
              Frame = llvm::dyn_cast<llvm::AllocaInst>(&I);
        if (!Frame)
          return false;
        for (auto &Block : F)
          if (auto *Invoke =
                  llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
              Invoke && Invoke->getCalledFunction() &&
              Invoke->getCalledFunction()->getIntrinsicID() ==
                  llvm::Intrinsic::seh_scope_begin) {
            llvm::IRBuilder<> B(
                &*Invoke->getNormalDest()->getFirstInsertionPt());
            auto *GEP = llvm::cast<llvm::GetElementPtrInst>(
                B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(0x80000000)));
            GEP->setNoWrapFlags(llvm::GEPNoWrapFlags::noUnsignedSignedWrap());
            auto *Integer = B.CreatePtrToInt(GEP, B.getInt32Ty());
            auto *Canceled = B.CreateAdd(Integer, B.getInt32(0x80000000));
            B.CreateLoad(B.getInt32Ty(),
                         B.CreateIntToPtr(Canceled, B.getPtrTy()));
            return true;
          }
        return false;
      },
      "unproved LLVM escape or access");
  RejectMutation(
      "wrong dispatch try level",
      [](llvm::Function &F) {
        for (auto &B : F)
          for (auto &I : B) {
            auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            auto Bundle =
                Call ? Call->getOperandBundle(windows_eh_md::ProvenanceBundle)
                     : std::nullopt;
            const auto *Role =
                Bundle ? llvm::dyn_cast<llvm::ConstantInt>(
                             Bundle->Inputs[windows_eh_md::ProvenanceRole])
                       : nullptr;
            if (!Role ||
                Role->getZExtValue() !=
                    unsigned(
                        windows_eh_md::NativeProvenanceRole::RegionDispatch))
              continue;
            auto *Address = I.getNextNode();
            auto *Store = Address ? llvm::dyn_cast_or_null<llvm::StoreInst>(
                                        Address->getNextNode())
                                  : nullptr;
            if (!Store)
              return false;
            Store->setOperand(0,
                              llvm::ConstantInt::get(
                                  llvm::Type::getInt32Ty(F.getContext()), 42));
            return true;
          }
        return false;
      },
      "try-level synchronization");
  RejectMutation(
      "wrong exception pointer cell",
      [](llvm::Function &F) {
        for (auto &Helper : *F.getParent())
          for (auto &B : Helper)
            for (auto &I : B) {
              auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I);
              auto *Frame = GEP ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                      GEP->getPointerOperand())
                                : nullptr;
              if (Frame &&
                  Frame->getIntrinsicID() == llvm::Intrinsic::frameaddress &&
                  GEP->getNumIndices() == 1) {
                GEP->setOperand(
                    1, llvm::ConstantInt::get(
                           llvm::Type::getInt32Ty(F.getContext()), -16));
                return true;
              }
            }
        return false;
      },
      "exception pointers");
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  auto Buffer = llvm::MemoryBuffer::getFile(Path);
  ASSERT_TRUE(bool(Buffer));
  const auto Bytes = (*Buffer)->getBuffer();
  std::vector<uint8_t> Binary(Bytes.bytes_begin(), Bytes.bytes_end());
  COFFPatcher Patcher;
  const uint64_t CodeVA = Patcher.plannedExecSegmentVA(Binary, Arch::X86);
  ASSERT_NE(CodeVA, 0u);
  auto Resolve = [&](llvm::StringRef Symbol,
                     uint32_t) -> std::optional<uint64_t> {
    if (auto Cookie = findCOFFRegistrationRuntimeVA(Image, Symbol))
      return *Cookie;
    if (auto Address = parseNdDataSymbol(Symbol))
      return *Address;
    if (auto Address = parseNdCodePtrSymbol(Symbol))
      return *Address;
    for (const auto &Function : *Module) {
      llvm::SmallString<64> ObjectName;
      llvm::Mangler Mangler;
      Mangler.getNameWithPrefix(ObjectName, &Function, false);
      if (ObjectName != Symbol)
        continue;
      auto Address = rewrite_source::getOriginalVA(Function);
      if (!Address) {
        llvm::consumeError(Address.takeError());
        return std::nullopt;
      }
      return *Address;
    }
    return std::nullopt;
  };
  auto Compiled = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                       CodeVA, Resolve, Image.Base);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.Unresolved.empty());
  auto Rows = validateCOFFRegistrationSemanticRows(*Parent, EH, Compiled);
  ASSERT_FALSE(bool(Rows)) << llvm::toString(std::move(Rows));
  va_t Generated = 0;
  for (const auto &Owner : Compiled.SourceFunctionOwners)
    if (Owner.SourceFunction == Parent->getName())
      Generated = Owner.OwnerVA;
  ASSERT_GE(Generated, CodeVA);
  const std::pair<va_t, va_t> Mapping{EH.CodeRange.Begin, Generated};
  auto GuardUpdate = prepareCOFFGuardTables(Binary, Image, Compiled, {Mapping},
                                            CodeVA, Arch::X86, true);
  ASSERT_TRUE(bool(GuardUpdate)) << llvm::toString(GuardUpdate.takeError());
  ASSERT_TRUE(GuardUpdate->ApplyCF);
  ASSERT_GT(GuardUpdate->CFFunctionCount, 0u);
  auto Update = prepareCOFFRegistrationPatch(Binary, Image, Compiled, {Mapping},
                                             CodeVA, *Module, &*GuardUpdate);
  ASSERT_TRUE(bool(Update)) << llvm::toString(Update.takeError());
  ASSERT_TRUE(Update->Apply);
  {
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    llvm::IRBuilder<> B(&*Function->getEntryBlock().getFirstInsertionPt());
    B.CreateLoad(B.getInt32Ty(),
                 llvm::ConstantExpr::getIntToPtr(
                     B.getInt32(Image.Base + 0x1000), B.getPtrTy()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  {
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    llvm::IRBuilder<> Entry(&*Function->getEntryBlock().getFirstInsertionPt());
    auto *Spill = Entry.CreateAlloca(Entry.getInt32Ty());
    Entry.CreateStore(Entry.getInt32(Image.Base + 0x2000), Spill);
    bool Added = false;
    for (auto &Block : *Function)
      if (auto *Invoke =
              llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
          Invoke && Invoke->getCalledFunction() &&
          Invoke->getCalledFunction()->getIntrinsicID() ==
              llvm::Intrinsic::seh_scope_begin) {
        llvm::IRBuilder<> B(&*Invoke->getNormalDest()->getFirstInsertionPt());
        auto *Address = B.CreateLoad(B.getInt32Ty(), Spill);
        Address->setVolatile(true);
        auto *Value = B.CreateLoad(B.getInt32Ty(),
                                   B.CreateIntToPtr(Address, B.getPtrTy()));
        Value->setVolatile(true);
        Added = true;
        break;
      }
    ASSERT_TRUE(Added);
    ASSERT_FALSE(llvm::verifyModule(*AlteredModule, &llvm::errs()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  for (unsigned Origin : {0u, 1u, 2u, 3u, 4u}) {
    SCOPED_TRACE(
        Origin == 1   ? "raw image VA through selected globals"
        : Origin == 2 ? "raw image VA assembled with two partial stores"
        : Origin == 3 ? "raw image VA returned by a local helper"
        : Origin == 4 ? "raw image VA returned by selected local helpers"
                      : "raw image VA synthesized through two scalar spills");
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    llvm::IRBuilder<> Entry(&*Function->getEntryBlock().getFirstInsertionPt());
    auto *Left = Entry.CreateAlloca(Entry.getInt32Ty());
    auto *Right = Entry.CreateAlloca(Entry.getInt32Ty());
    if (Origin == 2) {
      Entry.CreateStore(Entry.getInt16((Image.Base + 0x1000) & 0xffff), Left);
      Entry.CreateStore(
          Entry.getInt16((Image.Base + 0x1000) >> 16),
          Entry.CreateGEP(Entry.getInt8Ty(), Left, Entry.getInt32(2)));
    } else
      Entry.CreateStore(Entry.getInt32(Image.Base / 2), Left);
    Entry.CreateStore(Entry.getInt32(Image.Base - Image.Base / 2 + 0x1000),
                      Right);
    bool Added = false;
    for (auto &Block : *Function)
      if (auto *Invoke =
              llvm::dyn_cast<llvm::InvokeInst>(Block.getTerminator());
          Invoke && Invoke->getCalledFunction() &&
          Invoke->getCalledFunction()->getIntrinsicID() ==
              llvm::Intrinsic::seh_scope_begin) {
        llvm::IRBuilder<> B(&*Invoke->getNormalDest()->getFirstInsertionPt());
        auto *A = B.CreateLoad(B.getInt32Ty(), Left);
        auto *C = B.CreateLoad(B.getInt32Ty(), Right);
        llvm::Value *Address = Origin == 2 ? A : B.CreateAdd(A, C);
        if (Origin == 1) {
          auto *Raw = new llvm::GlobalVariable(
              *AlteredModule, B.getInt32Ty(), true,
              llvm::GlobalValue::PrivateLinkage,
              B.getInt32(Image.Base + 0x1000), "raw.image.va");
          auto *Zero = new llvm::GlobalVariable(
              *AlteredModule, B.getInt32Ty(), true,
              llvm::GlobalValue::PrivateLinkage, B.getInt32(0), "zero.va");
          auto *Selected = B.CreateSelect(B.CreateICmpEQ(A, C), Raw, Zero);
          Address = B.CreateLoad(B.getInt32Ty(), Selected);
        }
        if (Origin >= 3) {
          auto *Type = llvm::FunctionType::get(B.getInt32Ty(), false);
          auto Helper = [&](llvm::StringRef Name, uint32_t Value) {
            auto *F = llvm::Function::Create(
                Type, llvm::GlobalValue::InternalLinkage, Name, *AlteredModule);
            llvm::IRBuilder<> Body(
                llvm::BasicBlock::Create(Context, "entry", F));
            Body.CreateRet(Body.getInt32(Value));
            return F;
          };
          llvm::Value *Target = Helper("raw.image.helper", Image.Base + 0x1000);
          if (Origin == 4)
            Target = B.CreateSelect(B.CreateICmpEQ(A, C), Target,
                                    Helper("zero.helper", 0));
          Address = B.CreateCall(Type, Target);
        }
        auto *Value = B.CreateLoad(B.getInt32Ty(),
                                   B.CreateIntToPtr(Address, B.getPtrTy()));
        Value->setVolatile(true);
        Added = true;
        break;
      }
    ASSERT_TRUE(Added);
    ASSERT_FALSE(llvm::verifyModule(*AlteredModule, &llvm::errs()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  {
    auto AlteredModule = llvm::CloneModule(*Module);
    auto *Function = AlteredModule->getFunction(Parent->getName());
    auto *EntryBranch = llvm::cast<llvm::UncondBrInst>(
        Function->getEntryBlock().getTerminator());
    auto *OriginalEntry = EntryBranch->getSuccessor(0);
    auto *Loop = llvm::BasicBlock::Create(Context, "numeric.origin.loop",
                                          Function, OriginalEntry);
    EntryBranch->setSuccessor(0, Loop);
    llvm::IRBuilder<> B(Loop);
    auto *Phi = B.CreatePHI(B.getInt32Ty(), 2);
    auto *Next =
        B.CreateAdd(Phi, B.getInt32(Image.Base - Image.Base / 2 + 0x1000));
    Phi->addIncoming(B.getInt32(Image.Base / 2), &Function->getEntryBlock());
    Phi->addIncoming(Next, Loop);
    B.CreateLoad(B.getInt32Ty(), B.CreateIntToPtr(Phi, B.getPtrTy()));
    auto *Again = new llvm::GlobalVariable(*AlteredModule, B.getInt1Ty(), false,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, "numeric.repeat");
    B.CreateCondBr(B.CreateLoad(B.getInt1Ty(), Again), Loop, OriginalEntry);
    ASSERT_FALSE(llvm::verifyModule(*AlteredModule, &llvm::errs()));
    auto AlteredCompiled = Compiled;
    auto Failure = prepareCOFFRegistrationPatch(
        Binary, Image, AlteredCompiled, {Mapping}, CodeVA, *AlteredModule);
    ASSERT_FALSE(bool(Failure));
    EXPECT_NE(llvm::toString(Failure.takeError()).find("raw image pointer"),
              std::string::npos);
  }
  ASSERT_EQ(Patcher.appendExecSegment(Binary, Compiled.Bytes, kNdTextSection,
                                      Arch::X86),
            CodeVA);
  auto PE = locatePEHeaders(Binary.data(), Binary.size());
  std::optional<size_t> EntryOffset;
  const uint32_t EntryRVA = EH.CodeRange.Begin - Image.Base;
  forEachPESection(PE, [&](const PESectionFields &Section, uint16_t) {
    if (EntryRVA >= Section.VirtualAddress &&
        rangeInBounds(EntryRVA - Section.VirtualAddress, 5,
                      Section.SizeOfRawData))
      EntryOffset =
          Section.PointerToRawData + EntryRVA - Section.VirtualAddress;
  });
  ASSERT_TRUE(EntryOffset);
  Binary[*EntryOffset] = 0xe9;
  llvm::support::endian::write32le(
      Binary.data() + *EntryOffset + 1,
      uint32_t(Generated - EH.CodeRange.Begin - 5));
  auto GuardInstalled = applyCOFFGuardTableUpdate(Binary, Image, *GuardUpdate);
  ASSERT_FALSE(bool(GuardInstalled))
      << llvm::toString(std::move(GuardInstalled));
  auto Installed = applyCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Installed)) << llvm::toString(std::move(Installed));
  auto Checked = validateCOFFRegistrationPatch(Binary, *Update);
  ASSERT_FALSE(bool(Checked)) << llvm::toString(std::move(Checked));
  {
    auto Altered = Binary;
    auto NewPE = locatePEHeaders(Altered.data(), Altered.size());
    forEachPESection(NewPE, [&](const PESectionFields &Section, uint16_t) {
      if (Image.Base + Section.VirtualAddress == CodeVA)
        Altered[Section.PointerToRawData] ^= 1;
    });
    auto Failure = validateCOFFRegistrationPatch(Altered, *Update);
    ASSERT_TRUE(bool(Failure));
    EXPECT_NE(llvm::toString(std::move(Failure)).find("differs"),
              std::string::npos);
  }
  {
    Patcher.setImageContext(&Image);
    const auto ProductPath =
        std::filesystem::path(Path).parent_path() / "product-patched.exe";
    const auto Result = Patcher.patch(Path, ProductPath, *Module, Arch::X86);
    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.TrampolineCount, 1u);
    // Exact source-callee identity must win even when its spelling aliases an
    // import. Otherwise a direct call can jump into an IAT slot as code.
    auto CollisionModule = llvm::CloneModule(*Module);
    llvm::Function *OriginalCallee = nullptr;
    auto *CollisionParent = CollisionModule->getFunction(Parent->getName());
    ASSERT_TRUE(CollisionParent);
    for (auto &Block : *CollisionParent)
      for (auto &I : Block) {
        auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        auto *Callee = Call ? Call->getCalledFunction() : nullptr;
        if (!Callee || !Callee->isDeclaration() || Callee->isIntrinsic())
          continue;
        auto Address = rewrite_source::getOriginalVA(*Callee);
        ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
        if (*Address) {
          ASSERT_TRUE(!OriginalCallee || OriginalCallee == Callee);
          OriginalCallee = Callee;
        }
      }
    ASSERT_TRUE(OriginalCallee);
    ASSERT_EQ(CollisionModule->getFunction("RaiseException"), nullptr);
    OriginalCallee->setName("RaiseException");
    const auto CollisionPath =
        std::filesystem::path(Path).parent_path() / "collision-patched.exe";
    const auto CollisionResult =
        Patcher.patch(Path, CollisionPath, *CollisionModule, Arch::X86);
    ASSERT_TRUE(CollisionResult.Success);
    EXPECT_EQ(CollisionResult.TrampolineCount, 1u);
  }
  if (const char *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_PE32")) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Output, EC);
    ASSERT_FALSE(bool(EC)) << EC.message();
    Stream.write(reinterpret_cast<const char *>(Binary.data()), Binary.size());
  }
#endif
}

struct RegistrationNativeFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"registration", Context};
  MedFunc Source;
  MedLLVMEmitter Emitter;
  llvm::Function *Parent = nullptr;
  llvm::BasicBlock *Entry = nullptr;
  llvm::BasicBlock *Try = nullptr;
  llvm::BasicBlock *Filter = nullptr;
  llvm::BasicBlock *Handler = nullptr;
  std::map<int, llvm::BasicBlock *> Blocks;

  RegistrationNativeFixture() {
    Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
    Module.setDataLayout("e-m:x-p:32:32-i64:64-f80:32-n8:16:32-a:0:32-S32");
    Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false),
        llvm::GlobalValue::ExternalLinkage, "guarded", Module);
    rewrite_source::setOriginalVA(*Parent, 0x1000);
    Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    Try = llvm::BasicBlock::Create(Context, "body", Parent);
    Filter = llvm::BasicBlock::Create(Context, "filter", Parent);
    Handler = llvm::BasicBlock::Create(Context, "handler", Parent);
    Blocks = {{0, Entry}, {1, Try}, {2, Filter}, {3, Handler}};
    Source.Entry = 0x1000;
    Source.RegistrationCallerCleanupABIComplete = true;
    Source.ExceptionMetadata.emplace();
    auto &EH = *Source.ExceptionMetadata;
    EH.Kind = RuntimeFunctionKind::Primary;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.PersonalityVA = 0x2000;
    EH.HandlerDataVA = 0x3000;
    EH.CodeRange = {0x1000, 0x1040};
    auto &Chain = EH.Registration.emplace();
    Chain.HandlerVA = EH.PersonalityVA;
    Chain.ScopeTableVA = EH.HandlerDataVA;
    Chain.RegistrationOffset = -16;
    Chain.TryLevelOffset = -4;
    Chain.SeededTryLevel = -1;
    Chain.ChainInstallVA = 0x1002;
    Chain.ChainRemoveVA = 0x1032;
    Chain.TryLevelStores = {{0x1004, 0x1010, 0}, {0x1014, 0x1015, -1}};
    Chain.Scopes = {{-1, 0x1020, 0x1030, false}};
    auto &States = Source.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = States.ChainOperationsComplete = true;
    const int32_t Levels[] = {-1, 0, -1, -1};
    for (int I = 0; I != 4; ++I) {
      MedBlock B;
      B.Id = I;
      B.StartAddr = 0x1000 + I * 0x10;
      B.EndAddr = B.StartAddr + 0x10;
      Source.Blocks.push_back(B);
      States.Blocks.push_back(
          {I, {B.StartAddr, B.EndAddr}, {Levels[I]}, false, I == 2, I != 2});
    }
    States.ChainAccesses = {
        {0x1001, 0x1002, 1, RegistrationChainAccess::Kind::ReadPreviousHead},
        {0x1002, 0x1003, 2, RegistrationChainAccess::Kind::Install},
        {0x1032, 0x1033, 3, RegistrationChainAccess::Kind::Remove}};
    llvm::IRBuilder<> B(Entry);
    auto *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(128), "frame");
    auto *FS = llvm::ConstantPointerNull::get(B.getPtrTy(257));
    auto *Prev = B.CreateLoad(B.getInt32Ty(), FS);
    auto *Install = B.CreateStore(B.getInt32(0), FS);
    B.CreateBr(Try);
    MedLLVMEmitterTestPeer::prepare(Emitter, Module, *Parent, Source, *Frame);
    MedLLVMEmitterTestPeer::chain(Emitter, *Prev, 0x1001, 1);
    MedLLVMEmitterTestPeer::chain(Emitter, *Install, 0x1002, 2);
    B.SetInsertPoint(Try);
    auto Callee = Module.getOrInsertFunction("raise", B.getVoidTy());
    auto *Call = B.CreateCall(Callee);
    MedLLVMEmitterTestPeer::call(Emitter, *Call, 0x1011);
    auto *Memory = B.CreateIntToPtr(B.getInt32(0x4000), B.getPtrTy());
    B.CreateLoad(B.getInt32Ty(), Memory, "source.faulting.load");
    B.CreateBr(Handler);
    B.SetInsertPoint(Filter);
    B.CreateRet(B.getInt32(1));
    B.SetInsertPoint(Handler);
    auto *Remove = B.CreateStore(B.getInt32(0), FS);
    MedLLVMEmitterTestPeer::chain(Emitter, *Remove, 0x1032, 3);
    B.CreateRet(B.getInt32(7));
  }
  std::string print() const {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Module.print(OS, nullptr);
    return Text;
  }
  bool lower() {
    return MedLLVMEmitterTestPeer::lower(Emitter, Source, *Parent, Blocks);
  }
};

TEST(WindowsRegistrationNative, ResolvesOnlyTheAuthenticatedEH4RuntimeSymbols) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.DynInfo.SecurityCookieRVA = 0x3000;
  Segment Text;
  Text.VA = 0x401000;
  Text.Size = 192;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Wrapper[] = {
      0x55, 0x89, 0xe5, 0xff, 0x75, 0x14, 0xff, 0x75, 0x10, 0xff, 0x75, 0x0c,
      0xff, 0x75, 0x08, 0x68, 0,    0,    0,    0,    0x68, 0,    0,    0,
      0,    0xff, 0x15, 0,    0,    0,    0,    0x83, 0xc4, 0x18, 0x5d, 0xc3};
  std::copy(std::begin(Wrapper), std::end(Wrapper), Text.Data.begin());
  llvm::support::endian::write32le(Text.Data.data() + 16, 0x401080);
  llvm::support::endian::write32le(Text.Data.data() + 21, 0x403000);
  llvm::support::endian::write32le(Text.Data.data() + 27, 0x403004);
  const uint8_t Check[] = {0x3b, 0x0d, 0x00, 0x30, 0x40,
                           0x00, 0x75, 0x10, 0xc3};
  std::copy(std::begin(Check), std::end(Check), Text.Data.begin() + 128);
  std::copy(std::begin(Check), std::end(Check), Text.Data.begin() + 160);
  Image.Segments.push_back(Text);
  Segment Data;
  Data.VA = 0x403000;
  Data.Size = 16;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(Data);
  Image.Imports.push_back(
      {"msvcrt.dll", "_except_handler4_common", 0, 0x403004});
  ExceptionFunction EH;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  EH.PersonalityVA = Text.VA;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Registration.emplace();
  Image.ExceptionMetadata.Functions.push_back(EH);
  EXPECT_EQ(findCOFFRegistrationRuntimeVA(Image, "___security_cookie"),
            0x403000);
  EXPECT_EQ(findCOFFRegistrationRuntimeVA(Image, "@__security_check_cookie@4"),
            0x401080);
  EXPECT_FALSE(findCOFFRegistrationRuntimeVA(Image, "__security_check_cookie"));
  auto Conflicting = Image;
  auto &Bytes = Conflicting.Segments.front().Data;
  std::copy(Bytes.begin(), Bytes.begin() + sizeof(Wrapper), Bytes.begin() + 64);
  llvm::support::endian::write32le(Bytes.data() + 64 + 16, 0x4010a0);
  EH.PersonalityVA += 64;
  Conflicting.ExceptionMetadata.Functions.push_back(EH);
  EXPECT_FALSE(
      findCOFFRegistrationRuntimeVA(Conflicting, "@__security_check_cookie@4"));
  EXPECT_EQ(findCOFFRegistrationRuntimeVA(Conflicting, "___security_cookie"),
            0x403000);
  Image.Imports.front().Module = "custom.dll";
  EXPECT_FALSE(findCOFFRegistrationRuntimeVA(Image, "___security_cookie"));
  EXPECT_FALSE(
      findCOFFRegistrationRuntimeVA(Image, "@__security_check_cookie@4"));
}

TEST(WindowsRegistrationNative, UsesCompilerOwnedScopesAndRecoveredCallbacks) {
  RegistrationNativeFixture F;
  ASSERT_TRUE(F.lower()) << F.print();
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  EXPECT_TRUE(F.Parent->hasPersonalityFn());
  EXPECT_TRUE(F.Parent->hasFnAttribute(llvm::Attribute::OptimizeNone));
  EXPECT_TRUE(
      F.Parent->hasFnAttribute("llvm.rewrite.win-x86-registration-state"));
  EXPECT_NE(F.Module.getFunction("guarded.registration.callback.0"), nullptr);
  const auto Text = F.print();
  EXPECT_NE(Text.find("invoke void @llvm.seh.scope.begin"), std::string::npos);
  EXPECT_NE(Text.find("invoke void @llvm.seh.scope.end"), std::string::npos);
  EXPECT_NE(Text.find("source.faulting.load = load volatile"),
            std::string::npos);
  EXPECT_EQ(Text.find("store i32 0, ptr addrspace(257) null"),
            std::string::npos);
}

TEST(WindowsRegistrationNative, NormalFinallyRequiresAnUnobservedSourceResult) {
  for (bool Observe : {false, true}) {
    RegistrationNativeFixture F;
    auto &Scope = F.Source.ExceptionMetadata->Registration->Scopes.front();
    Scope.IsFinally = true;
    Scope.FilterVA = 0;
    Scope.HandlerVA = 0x1020;
    F.Source.RegistrationStates->Blocks[2].CanDispatch = true;
    MedOp SourceCall;
    SourceCall.Opcode = NdOp::CALL;
    SourceCall.Addr = 0x1011;
    SourceCall.OriginSeq = 1;
    SourceCall.NumInputs = 1;
    SourceCall.Inputs[0] = MedVar::makeConst(0x1020, 4);
    F.Source.Blocks[1].Ops.push_back(SourceCall);
    auto *Old = llvm::cast<llvm::CallInst>(&F.Try->front());
    llvm::IRBuilder<> B(Old);
    auto *Call = B.CreateCall(
        F.Module.getOrInsertFunction("direct.finally", B.getInt64Ty()));
    MedLLVMEmitterTestPeer::forgetCall(F.Emitter, *Old);
    Old->eraseFromParent();
    MedLLVMEmitterTestPeer::call(F.Emitter, *Call, 0x1011);
    llvm::IRBuilder<> Entry(F.Entry->getTerminator());
    auto *Slot = Entry.CreateAlloca(B.getInt32Ty());
    B.SetInsertPoint(Call->getNextNode());
    B.CreateStore(B.CreateTrunc(Call, B.getInt32Ty()), Slot);
    if (Observe)
      B.CreateLoad(B.getInt32Ty(), Slot)->setVolatile(true);
    const auto Before = F.print();
    EXPECT_EQ(F.lower(), !Observe);
    if (Observe) {
      EXPECT_EQ(F.print(), Before);
      continue;
    }
    EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    unsigned NormalCalls = 0;
    for (const auto &Block : *F.Parent)
      for (const auto &I : Block)
        if (I.getMetadata(windows_eh_md::RegistrationFinallyCallAttachment)) {
          ++NormalCalls;
          const auto *Call = llvm::cast<llvm::CallBase>(&I);
          ASSERT_EQ(Call->arg_size(), 2u);
          EXPECT_EQ(llvm::cast<llvm::ConstantInt>(Call->getArgOperand(0))
                        ->getZExtValue(),
                    0u);
        }
    EXPECT_EQ(NormalCalls, 1u);
  }
}

TEST(WindowsRegistrationNative,
     RejectsAnUnauthenticatedFSOccurrenceAtomically) {
  RegistrationNativeFixture F;
  llvm::IRBuilder<> B(F.Try->getTerminator());
  B.CreateLoad(B.getInt32Ty(), llvm::ConstantPointerNull::get(B.getPtrTy(257)));
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsUnprovedStackCleanupAtomically) {
  for (bool CalleePop : {false, true}) {
    RegistrationNativeFixture F;
    if (CalleePop)
      F.Source.CalleePopBytes = 4;
    else
      F.Source.RegistrationCallerCleanupABIComplete = false;
    const auto Before = F.print();
    EXPECT_FALSE(F.lower());
    EXPECT_EQ(F.print(), Before);
  }
}

TEST(WindowsRegistrationNative, ReplaysPreservedCalleeReturnCleanup) {
  for (bool CalleePop : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.Name = ".text";
    Text.VA = 0x401000;
    Text.Size = 16;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(16, 0xcc);
    Text.Data[0] = CalleePop ? 0xc2 : 0xc3;
    Text.Data[1] = 4;
    Text.Data[2] = 0;
    Image.Segments.push_back(Text);
    LowFunc Source;
    Source.Entry = 0x402000;
    LowBlock Block;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(Text.VA, 4);
    Block.Ops.push_back(Call);
    Source.Blocks.push_back(Block);
    EXPECT_EQ(hasCallerCleanupRegistrationABI(Source, Image), !CalleePop);
    Source.CalleePopBytes = 4;
    EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
    Source.CalleePopBytes = 0;
    Source.Blocks.front().Ops.front().Opcode = NdOp::INDIR_CALL;
    EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
  }
}

TEST(WindowsRegistrationNative, RejectsPreservedCallerFrameAndFSObservers) {
  const std::vector<std::vector<uint8_t>> Programs = {
      // Follow the saved caller EBP, then read its try-level slot.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x8b, 0x40, 0xfc, 0x5d, 0xc3},
      // Observe the live thread registration chain without an explicit
      // argument.
      {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xc3},
      // An ordinary cdecl frame may read a private initialized local.
      {0x55, 0x89, 0xe5, 0x83, 0xec, 0x04, 0xc7, 0x45, 0xfc, 0x07,
       0x00, 0x00, 0x00, 0x8b, 0x45, 0xfc, 0x89, 0xec, 0x5d, 0xc3},
      // The same forbidden observation behind a second internal call.
      {0x55, 0x89, 0xe5, 0xe8, 0x08, 0x00, 0x00, 0x00, 0x89, 0xec, 0x5d, 0xc3,
       0xcc, 0xcc, 0xcc, 0xcc, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xc3},
      // A closed, frame-private internal callee remains supported.
      {0x55, 0x89, 0xe5, 0xe8, 0x08, 0x00, 0x00, 0x00, 0x89, 0xec, 0x5d, 0xc3,
       0xcc, 0xcc, 0xcc, 0xcc, 0xc3},
      // Atomic RMWs must not bypass caller-frame privacy.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0xf0, 0x87, 0x48, 0xfc, 0x5d, 0xc3},
      // A saved caller frame cannot escape in a branch condition.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x85, 0xc0, 0x74, 0x02, 0x5d, 0xc3,
       0x5d, 0xc3},
      // Unbounded positive offsets can reach the parent's registration.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x24, 0x5d, 0xc3},
      // A saved caller frame cannot be passed in an outgoing stack slot.
      {0x55, 0x89, 0xe5, 0xff, 0x75, 0x00, 0xe8, 0x05, 0x00, 0x00, 0x00, 0x89,
       0xec, 0x5d, 0xc3, 0xcc, 0xc3},
      // Stack bytes below ESP are not initialized by this invocation.
      {0x8b, 0x44, 0x24, 0xf4, 0xa3, 0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0xc3},
      // Moving a saved caller frame through XMM0 retains its provenance.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x66, 0x0f, 0x6e,
       0xc0, 0x66, 0x0f, 0x7e, 0xc0, 0x8b, 0x40, 0xfc, 0xa3,
       0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0x5d, 0xc3},
      // Flags cannot hide a branch on the physical stack address.
      {0x55, 0x89, 0xe5, 0x89, 0xe0, 0xa9, 0x0f, 0x00, 0x00, 0x00,
       0x74, 0x04, 0x31, 0xc0, 0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3},
      // A preserved helper must restore the callee-saved registers and ESP.
      {0x31, 0xed, 0x31, 0xc0, 0xc3},
      {0x83, 0xec, 0x04, 0x31, 0xc0, 0xc3},
      // The return PC may identify the generated caller, but cannot select a
      // branch whose outcome changes with the generated instruction address.
      {0x8b, 0x04, 0x24, 0xa9, 0x01, 0x00, 0x00, 0x00, 0x74, 0x03, 0x31, 0xc0,
       0xc3, 0x31, 0xc0, 0xc3},
      // An external observer may record the real generated call site.
      {0x8b, 0x04, 0x24, 0xa3, 0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0xc3},
      // Reloading that global inside the checked closure cannot launder PC.
      {0x8b, 0x04, 0x24, 0xa3, 0x00, 0x30, 0x40, 0x00, 0xa1,
       0x00, 0x30, 0x40, 0x00, 0xa9, 0x01, 0x00, 0x00, 0x00,
       0x74, 0x03, 0x31, 0xc0, 0xc3, 0x31, 0xc0, 0xc3},
      // The same reload in a second callee belongs to the same closure.
      {0x8b, 0x04, 0x24, 0xa3, 0x00, 0x30, 0x40, 0x00, 0xe8, 0x07,
       0x00, 0x00, 0x00, 0x31, 0xc0, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
       0xa1, 0x00, 0x30, 0x40, 0x00, 0x31, 0xc0, 0xc3},
      // An unallocated negative displacement is not private stack storage.
      {0x55, 0x89, 0xe5, 0x8b, 0x45, 0x00, 0x89, 0x85, 0x00, 0x00, 0xff, 0xff,
       0x31, 0xc0, 0x5d, 0xc3},
      // CALL invalidates a previously cleared volatile XMM register.
      {0x55, 0x89, 0xe5, 0x31, 0xc0, 0x66, 0x0f, 0x6e, 0xc0, 0xe8,
       0x12, 0x00, 0x00, 0x00, 0x66, 0x0f, 0x7e, 0xc0, 0xa3, 0x00,
       0x30, 0x40, 0x00, 0x31, 0xc0, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc,
       0xcc, 0xcc, 0x66, 0x0f, 0x6e, 0xc5, 0x31, 0xc0, 0xc3},
      // CALL also invalidates flags cleared before a frame-observing callee.
      {0x55, 0x89, 0xe5, 0x31, 0xc0, 0xe8, 0x16, 0x00, 0x00, 0x00, 0x74, 0x04,
       0x31, 0xc0, 0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc,
       0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x89, 0xe0, 0xa9, 0x0f,
       0x00, 0x00, 0x00, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xc3}};
  for (size_t Index = 0; Index != Programs.size(); ++Index) {
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Text;
    Text.Name = ".text";
    Text.VA = 0x401000;
    Text.Data = Programs[Index];
    Text.Size = Text.Data.size();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Segments.push_back(Text);
    Segment Data;
    Data.Name = ".data";
    Data.VA = 0x403000;
    Data.Data.resize(8);
    Data.Size = Data.Data.size();
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(Data);
    LowFunc Source;
    Source.Entry = 0x402000;
    Source.Blocks.emplace_back();
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.NumInputs = 1;
    Call.Inputs[0] = NdVar::cst(Text.VA, 4);
    Source.Blocks.front().Ops.push_back(Call);
    EXPECT_EQ(hasCallerCleanupRegistrationABI(Source, Image),
              Index == 2 || Index == 4 || Index == 15)
        << Index;
    if (Index == 15) {
      auto &States = Source.RegistrationStates.emplace();
      States.ImageReadsComplete = true;
      States.ImageReads = {{0x403002, 0x403003}};
      EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
      States.ImageReads = {{0x403004, 0x403008}};
      EXPECT_TRUE(hasCallerCleanupRegistrationABI(Source, Image));
      States.ImageReadsComplete = false;
      EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
    }
    if (Index == 2) {
      Source.Entry = Text.VA;
      EXPECT_FALSE(hasCallerCleanupRegistrationABI(Source, Image));
    }
  }
}

TEST(WindowsRegistrationNative, RejectsCallbackChainObservationsAtomically) {
  RegistrationNativeFixture F;
  llvm::IRBuilder<> B(F.Filter->getTerminator());
  auto *Read = B.CreateLoad(B.getInt32Ty(),
                            llvm::ConstantPointerNull::get(B.getPtrTy(257)));
  MedLLVMEmitterTestPeer::chain(F.Emitter, *Read, 0x1021, 4);
  F.Source.RegistrationStates->ChainAccesses.push_back(
      {0x1021, 0x1022, 4, RegistrationChainAccess::Kind::ReadInstalledHead});
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsAnAtomicFSOccurrenceAtomically) {
  RegistrationNativeFixture F;
  llvm::IRBuilder<> B(F.Try->getTerminator());
  B.CreateAtomicRMW(llvm::AtomicRMWInst::Add,
                    llvm::ConstantPointerNull::get(B.getPtrTy(257)),
                    B.getInt32(1), llvm::Align(4),
                    llvm::AtomicOrdering::SequentiallyConsistent);
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsFramePointersPassedToUnknownCallees) {
  RegistrationNativeFixture F;
  MedOp Root;
  Root.Opcode = NdOp::COPY;
  Root.Addr = 0x1011;
  Root.OriginSeq = 10;
  Root.Output.Kind = MedVar::Temp;
  Root.Output.Id = 12;
  Root.Output.SSAVer = 1;
  Root.Output.Size = 4;
  F.Source.Blocks[1].Ops.push_back(Root);
  F.Source.RegistrationStates->FrameValues.push_back({0x1011, 10, -4});
  MedCallInfo Call;
  Call.BlockId = 1;
  Call.Args.push_back(Root.Output);
  F.Source.CallInfos.push_back(Call);
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, RejectsFramePointerEscapeThroughAPhi) {
  RegistrationNativeFixture F;
  MedOp Root;
  Root.Opcode = NdOp::COPY;
  Root.Addr = 0x1011;
  Root.OriginSeq = 10;
  Root.Output.Kind = MedVar::Temp;
  Root.Output.Id = 12;
  Root.Output.SSAVer = 1;
  Root.Output.Size = 4;
  F.Source.Blocks[1].Ops.push_back(Root);
  F.Source.RegistrationStates->FrameValues.push_back(
      {0x1011, 10, std::nullopt});
  PhiNode Phi;
  Phi.Output = Root.Output;
  Phi.Output.Id = 13;
  Phi.Args.emplace_back(1, Root.Output);
  F.Source.Blocks[3].Phis.push_back(Phi);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Phi.Output);
  F.Source.Blocks[3].Ops.push_back(Return);
  const auto Before = F.print();
  EXPECT_FALSE(F.lower());
  EXPECT_EQ(F.print(), Before);
}

TEST(WindowsRegistrationNative, SynchronizesRuntimeTryLevelBeforeHandlerEntry) {
  RegistrationNativeFixture F;
  ASSERT_TRUE(F.lower());
  const auto Text = F.print();
  EXPECT_NE(Text.find("store volatile i32 -1, ptr"), std::string::npos);
  EXPECT_NE(Text.find("i32 56"), std::string::npos);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_EH
CompiledImage compileFixture(RegistrationNativeFixture &F) {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  return compileImageForPatch(
      F.Module, Arch::X86, BinaryFormat::COFF, 0x6000,
      [](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "__except_handler3" || Symbol == "__except_handler4")
          return 0x2000;
        if (Symbol == "___security_cookie")
          return 0x2500;
        if (Symbol == "@__security_check_cookie@4")
          return 0x2600;
        if (Symbol == "_raise")
          return 0x5000;
        return std::nullopt;
      },
      0x1000, "i686-pc-windows-msvc");
}

TEST(WindowsRegistrationNative, CompilerClosesTheIndexedEH3ScopeTable) {
  RegistrationNativeFixture F;
  ASSERT_TRUE(F.lower());
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.WinEHSemanticsValid);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Row.Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH3);
  EXPECT_EQ(Row.GeneratedState, 0u);
  EXPECT_EQ(Row.EnclosingState, -1);
  EXPECT_EQ(Row.RecordVA, Row.ContainerVA);
  EXPECT_EQ(Row.ContainerEndVA, Row.ContainerVA + 12);
  EXPECT_NE(Row.FilterVA, 0u);
  EXPECT_NE(Row.HandlerVA, 0u);
  auto Error = validateCOFFRegistrationSemanticRows(
      *F.Parent, *F.Source.ExceptionMetadata, Compiled);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}

TEST(WindowsRegistrationNative, AuthenticatesEveryEH4CookieFrameOffset) {
  RegistrationNativeFixture F;
  auto &EH = *F.Source.ExceptionMetadata;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.GSCookieOffset = -2;
  Chain.EHCookieOffset = -20;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Source.RegistrationStates->SecurityCookiesComplete = true;
  F.Source.RegistrationStates->SecurityCookieVA = 0x2500;
  for (auto &State : F.Source.RegistrationStates->Blocks)
    for (auto &Level : State.Levels)
      if (Level == -1)
        Level = -2;
  ASSERT_TRUE(F.lower());
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Row.Encoding,
            llvm::mc_rewrite::RewriteWinEHSemanticEncoding::X86SEH4);
  EXPECT_EQ(Row.RegistrationCookieOffsets[0], -2);
  EXPECT_LT(Row.RegistrationCookieOffsets[2], 0);
  auto Valid = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  // A different, plausible negative offset used to pass a sign-only check.
  llvm::support::endian::write32le(
      Compiled.Bytes.data() + Row.ContainerVA - Compiled.BaseVA + 8,
      uint32_t(Row.RegistrationCookieOffsets[2] - 4));
  auto Altered = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_TRUE(bool(Altered));
  EXPECT_NE(llvm::toString(std::move(Altered)).find("machine frame offsets"),
            std::string::npos);
}

#ifdef LLVM_NEVERD_X86_REGISTRATION_GS
TEST(WindowsRegistrationNative, EH4GSUsesTheCompilerCookieAndFastcallChecker) {
  RegistrationNativeFixture F;
  for (auto &I : *F.Entry)
    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
        Alloca && Alloca->getName() == "frame")
      Alloca->setAlignment(llvm::Align(64));
  auto &EH = *F.Source.ExceptionMetadata;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.GSCookieOffset = -32;
  Chain.EHCookieOffset = -28;
  Chain.HasSecurityCookies = true;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Source.RegistrationStates->SecurityCookiesComplete = true;
  F.Source.RegistrationStates->SecurityCookieVA = 0x2500;
  for (auto &State : F.Source.RegistrationStates->Blocks)
    for (auto &Level : State.Levels)
      if (Level == -1)
        Level = -2;
  ASSERT_TRUE(F.lower());
  EXPECT_TRUE(F.Parent->hasFnAttribute(llvm::Attribute::StackProtectReq));
  const auto *Check = F.Module.getFunction("__security_check_cookie");
  ASSERT_NE(Check, nullptr);
  EXPECT_TRUE(hasX86RegistrationSecurityCheckABI(*Check));
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_NE(Row.RegistrationCookieOffsets[0], -2);
  EXPECT_EQ(Row.RegistrationCookieOffsets[1], 0);
  auto Valid = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
}
#endif

TEST(WindowsRegistrationNative, BindsGeneratedParentsToTheSourceScopeGraph) {
  RegistrationNativeFixture F;
  auto &EH = *F.Source.ExceptionMetadata;
  EH.CodeRange.End = 0x1050;
  EH.Registration->Scopes.push_back({0, 0x1020, 0x1040, false});
  EH.Registration->TryLevelStores.front().Level = 1;
  F.Source.RegistrationStates->Blocks[1].Levels = {1};
  F.Source.RegistrationStates->Blocks.push_back(
      {4, {0x1040, 0x1050}, {0}, false, false, true});
  MedBlock Handler;
  Handler.Id = 4;
  Handler.StartAddr = 0x1040;
  Handler.EndAddr = 0x1050;
  F.Source.Blocks.push_back(Handler);
  auto *InnerHandler =
      llvm::BasicBlock::Create(F.Context, "inner.handler", F.Parent);
  llvm::IRBuilder<>(InnerHandler).CreateBr(F.Handler);
  F.Blocks.emplace(4, InnerHandler);
  ASSERT_TRUE(F.lower());
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 2u);
  auto Valid = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_FALSE(bool(Valid)) << llvm::toString(std::move(Valid));
  for (auto &Row : Compiled.WinEHSemanticRecords)
    if (Row.Token.Region == 1) {
      Row.EnclosingState = -1;
      llvm::support::endian::write32le(
          Compiled.Bytes.data() + Row.RecordVA - Compiled.BaseVA, UINT32_MAX);
    }
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteWinEHSemanticRecords(
      Compiled.WinEHSemanticRecords, Compiled.SourceFunctionOwners,
      Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs));
  auto Altered = validateCOFFRegistrationSemanticRows(*F.Parent, EH, Compiled);
  ASSERT_TRUE(bool(Altered));
  EXPECT_NE(llvm::toString(std::move(Altered)).find("source scope graph"),
            std::string::npos);
}

TEST(WindowsRegistrationNative, CompilerOwnsTheFinallyTableTarget) {
  RegistrationNativeFixture F;
  auto &Scope = F.Source.ExceptionMetadata->Registration->Scopes.front();
  Scope.IsFinally = true;
  Scope.FilterVA = 0;
  Scope.HandlerVA = 0x1020;
  F.Source.RegistrationStates->Blocks[2].CanDispatch = true;
  ASSERT_TRUE(F.lower());
  auto Compiled = compileFixture(F);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_EQ(Compiled.WinEHSemanticRecords.size(), 1u);
  const auto &Row = Compiled.WinEHSemanticRecords.front();
  EXPECT_EQ(Row.FilterVA, 0u);
  EXPECT_NE(Row.HandlerVA, Compiled.SourceFunctionOwners.back().OwnerVA);
  auto Error = validateCOFFRegistrationSemanticRows(
      *F.Parent, *F.Source.ExceptionMetadata, Compiled);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
}
#endif
} // namespace

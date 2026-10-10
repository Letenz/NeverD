//===- WindowsRegistrationRealignedTests.cpp - PE32 callback recovery -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>

using namespace neverd;

namespace {

#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
TEST(WindowsRegistrationRealigned, EmitsIndependentCallbackFrame) {
  llvm::LLVMContext Context;
  llvm::Module Module("callback-probe", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  llvm::IRBuilder<> B(Context);
  auto *Parent = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "callback_parent", Module);
  Parent->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
  Parent->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
  Parent->addFnAttr("frame-pointer", "all");
  Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      Module
          .getOrInsertFunction("__CxxFrameHandler3",
                               llvm::FunctionType::get(B.getInt32Ty(), true))
          .getCallee()));
  auto MakeBlock = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Parent);
  };
  auto *Entry = MakeBlock("entry"), *Normal = MakeBlock("normal"),
       *Dispatch = MakeBlock("dispatch"), *Catch = MakeBlock("catch"),
       *Resume = MakeBlock("resume");
  B.SetInsertPoint(Entry);
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 512));
  Frame->setAlignment(llvm::Align(64));
  B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                   &Module, llvm::Intrinsic::localescape),
               {Frame});
  B.CreateStore(B.getInt32(2), Frame)->setVolatile(true);
  auto Throw = Module.getOrInsertFunction(
      "callback_throw", llvm::FunctionType::get(B.getVoidTy(), false));
  B.CreateInvoke(Throw, Normal, Dispatch);
  B.SetInsertPoint(Normal);
  B.CreateUnreachable();
  B.SetInsertPoint(Dispatch);
  auto *Switch =
      B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Catch);
  B.SetInsertPoint(Catch);
  auto *Null = llvm::ConstantPointerNull::get(B.getPtrTy());
  auto *Pad = B.CreateCatchPad(Switch, {Null, B.getInt32(64), Null});
  auto Increment = Module.getOrInsertFunction(
      "callback_increment",
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false));
  llvm::cast<llvm::Function>(Increment.getCallee())
      ->setCallingConv(llvm::CallingConv::X86_ThisCall);
  auto *Call = B.CreateCall(Increment, {Frame},
                            {llvm::OperandBundleDef("funclet", Pad)});
  Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
  Call->setDoesNotThrow();
  B.CreateCatchRet(Pad, Resume);
  B.SetInsertPoint(Resume);
  auto *Result = B.CreateLoad(B.getInt32Ty(), Frame);
  Result->setVolatile(true);
  B.CreateRet(Result);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  auto Object = Codegen().compile(Module, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Object.Success);
  if (const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_OBJECT")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
              Object.ObjectData.size());
    Out.close();
    ASSERT_FALSE(Out.has_error());
  }
}

TEST(WindowsRegistrationRealigned, InputPE32RecoversTheCallbackContract) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_PE32");
  if (!Path)
    GTEST_SKIP()
        << "run check_windows_registration_realigned.py for the PE32 fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image));
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions, [](const auto &EH) {
        return EH.Registration && EH.Registration->RealignedFrame;
      });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  const auto EH = *It;
  ASSERT_TRUE(EH.Cxx);
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(EH.Cxx->TryBlocks[0].Handlers.size(), 1u);
  const auto Catch = EH.Cxx->TryBlocks[0].Handlers[0].HandlerVA;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto F =
      CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin, "callback_parent");
  ASSERT_TRUE(F.RegistrationStates);
  const auto &State = *F.RegistrationStates;
  EXPECT_TRUE(State.Complete);
  EXPECT_TRUE(State.CallbackStatesComplete);
  EXPECT_TRUE(State.RegistrationLifetimeComplete);
  EXPECT_TRUE(State.ChainOperationsComplete);
  EXPECT_TRUE(State.CallFrameEffectsComplete);
  EXPECT_TRUE(State.ImageReadsComplete);
  ASSERT_TRUE(State.CxxContinuationsComplete);
  ASSERT_EQ(State.CxxContinuations.size(), 1u);
  EXPECT_EQ(State.CxxContinuations[0].SavedStackOffset,
            EH.Registration->RealignedFrame->BaseOffset);
  EXPECT_TRUE(llvm::any_of(State.Blocks, [&](const auto &B) {
    return B.Range.Begin == State.CxxContinuations[0].TargetVA && B.Reached &&
           !B.Unknown && !B.CallbackOnly;
  }));
  const auto Med =
      LowToMedConverter().convert(F, Arch::X86, BinaryFormat::COFF);
  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 0u);
  EXPECT_GT(High.UnstructuredExceptionRegions, 0u);
  ASSERT_EQ(High.Body.size(), 1u);
  EXPECT_FALSE(High.Body.front().EHIsReducible);
  ASSERT_EQ(High.Body.front().EHClauses.size(), 1u);
  EXPECT_EQ(High.Body.front().EHClauses[0].ContinuationVAs,
            (std::vector<va_t>{State.CxxContinuations[0].TargetVA}));

  // Source callback analysis does not grant a new native frame/ABI model.
  EXPECT_FALSE(classifyWindowsEHNativeSource(EH, Arch::X86, BinaryFormat::COFF)
                   .canPatchOutput());

  auto Change = [](BinaryImage &Img, va_t Address, uint8_t Byte) {
    for (auto &Segment : Img.Segments)
      if (Address >= Segment.VA && Address - Segment.VA < Segment.Data.size()) {
        Segment.Data[Address - Segment.VA] = Byte;
        return true;
      }
    return false;
  };
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Bad = *Image;
    if (Mutation == 0)
      ASSERT_TRUE(Change(Bad, Catch, 0x50)); // Save EAX instead of runtime EBP.
    if (Mutation == 1)
      ASSERT_TRUE(Change(Bad, State.CxxContinuations[0].Address - 1, 0x58));
    if (Mutation == 2)
      ASSERT_TRUE(Change(Bad, Catch + 3, Bad.readVA(Catch + 3, 1)[0] + 4));
    if (Mutation == 3) {
      // A metadata snapshot alone cannot authenticate a changed allocation.
      const auto Address = EH.CodeRange.Begin + 11;
      ASSERT_TRUE(Change(Bad, Address, Bad.readVA(Address, 1)[0] ^ 4));
    }
    Decoder BadDecode;
    ASSERT_TRUE(BadDecode.init(Bad));
    const auto Broken = CFGBuilder().build(Bad, BadDecode, EH.CodeRange.Begin,
                                           "changed_callback_parent");
    ASSERT_TRUE(Broken.RegistrationStates);
    EXPECT_FALSE(Broken.RegistrationStates->CxxContinuationsComplete &&
                 Broken.RegistrationStates->ImageReadsComplete &&
                 Broken.RegistrationStates->ChainOperationsComplete);
  }

  auto Ordinary = F;
  Ordinary.OrdinaryModuleAnalysisRoots.insert(Catch);
  const auto OrdinaryStates = analyzeRegistrationStates(Ordinary);
  EXPECT_FALSE(OrdinaryStates.CxxContinuationsComplete);
  EXPECT_FALSE(OrdinaryStates.ChainOperationsComplete);

  // Both allocator choices must retain the exact chain-head read/store pair.
  const auto Read = EH.Registration->ChainInstallVA - 13;
  const auto ModRM = Image->readVA(Read, 3)[2];
  ASSERT_TRUE(ModRM == 0x0d || ModRM == 0x15);
  for (bool Matched : {false, true}) {
    BinaryImage Variant = *Image;
    ASSERT_TRUE(Change(Variant, Read + 2, ModRM == 0x0d ? 0x15 : 0x0d));
    if (Matched)
      ASSERT_TRUE(Change(Variant, Read + 8, ModRM == 0x0d ? 0x96 : 0x8e));
    Variant.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Variant);
    const auto *Graph =
        Variant.ExceptionMetadata.findFunction(EH.CodeRange.Begin);
    ASSERT_NE(Graph, nullptr);
    ASSERT_TRUE(Graph->Registration);
    EXPECT_EQ(Graph->Registration->RealignedFrame.has_value(), Matched);
  }
}

#else
TEST(WindowsRegistrationRealigned, RequiresCompilerFrameSupport) {
  GTEST_SKIP() << "LLVM does not provide the checked PE32 C++ frame ABI";
}
#endif

} // namespace

//===- RegistrationSEHParentExitTests.cpp - Finally parent-exit identity --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tests source parent-exit identity, chain lifetime and failed projection.

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHParentExit.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationCallback.h"
#include "neverd/ir/med/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86RegistrationSEHParentExit.h"
#include "neverd/ir/med/X86RegistrationSEHStateStore.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>

using namespace neverd;

namespace {

MedFunc parentExit(bool Runtime) {
  MedFunc Function;
  Function.Entry = 0x1000;
  auto &EH = Function.ExceptionMetadata.emplace();
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
  EH.CodeRange = {0x1000, 0x1900};
  auto &Chain = EH.Registration.emplace();
  Chain.RegistrationOffset = -16;
  Chain.Scopes = {{-1, 0, 0x1800, true}};
  auto &States = Function.RegistrationStates.emplace();
  States.Complete = States.CallbackStatesComplete =
      States.RegistrationLifetimeComplete = true;
  constexpr va_t Addresses[] = {0x1000, 0x1800, 0x1100};
  for (int Id = 0; Id != 3; ++Id) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Addresses[Id];
    Block.EndAddr = Addresses[Id] + 5;
    Function.Blocks.push_back(Block);
    RegistrationBlockState State;
    State.BlockId = Id;
    State.Range = {Block.StartAddr, Block.EndAddr};
    State.Reached = true;
    State.CallbackOnly = Id == 1;
    States.Blocks.push_back(State);
  }
  auto &Call = Function.Blocks[0];
  Call.Succs = {2};
  MedOp Invoke;
  Invoke.Opcode = NdOp::CALL;
  Invoke.Addr = 0x1000;
  Invoke.OriginSeq = 0;
  Invoke.addInput(MedVar::makeConst(0x1800, 4));
  Call.Ops.push_back(Invoke);
  auto &Finally = Function.Blocks[1];
  Finally.Succs = {2};
  MedOp Restore;
  Restore.Opcode = NdOp::LOAD;
  Restore.Addr = 0x1800;
  Restore.OriginSeq = 1;
  Restore.Output.Kind = MedVar::Reg;
  Restore.Output.TheArch = Arch::X86;
  Restore.Output.Size = 4;
  Restore.Output.RegOff = x86reg::RSP;
  MedVar Pointer;
  Pointer.Size = 4;
  Restore.addInput(Pointer);
  Finally.Ops.push_back(Restore);
  MedOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = 0x1803;
  Branch.OriginSeq = 0;
  Branch.addInput(MedVar::makeConst(0x1100, 8));
  Finally.Ops.push_back(Branch);
  States.FrameValues.push_back({0x1800, 1, -64});
  RegistrationSEHInvocationExit Exit;
  Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentJump;
  Exit.EntryVA = 0x1800;
  Exit.CallVA = Runtime ? 0 : 0x1000;
  Exit.Address = 0x1803;
  Exit.EndAddress = 0x1805;
  Exit.OpSeq = 0;
  Exit.TargetVA = 0x1100;
  Exit.StackRestoreVA = 0x1800;
  Exit.StackRestoreSeq = 1;
  Exit.StackOffset = -64;
  States.SEHInvocationExits.push_back(Exit);
  return Function;
}

} // namespace

TEST(RegistrationSEHParentExit, BindsOrdinaryAndRuntimeSourceExits) {
  for (bool Runtime : {false, true}) {
    auto Function = parentExit(Runtime);
    auto Exits = getRegistrationSEHParentExits(Function);
    ASSERT_TRUE(Exits);
    ASSERT_EQ(Exits->size(), 1u);
    EXPECT_EQ(Exits->at(1).EntryVA, 0x1800u);
    EXPECT_EQ(Exits->at(1).TargetBlock, 2);
    EXPECT_EQ(Exits->at(1).RestoreBlock, 1);
    EXPECT_EQ(Exits->at(1).StackOffset, -64);
    EXPECT_EQ(Exits->at(1).Branch, &Function.Blocks[1].Ops[1]);
    EXPECT_EQ(Exits->at(1).Restore, &Function.Blocks[1].Ops[0]);
    EXPECT_EQ(Exits->at(1).Invocations.size(), 1u);
    auto Owners = registrationSEHCallbackOwners(Function);
    ASSERT_TRUE(Owners);
    EXPECT_EQ(*Owners,
              (std::map<int, std::pair<va_t, bool>>{{1, {0x1800, true}}}));
    std::reverse(Function.Blocks.begin(), Function.Blocks.end());
    EXPECT_EQ(registrationSEHCallbackOwners(Function), Owners);
    EXPECT_TRUE(getRegistrationSEHParentExits(Function));
    // A source parent branch is not the callback's ordinary C return ABI.
    const auto Returns = getRegistrationSEHReturns(Function);
    ASSERT_TRUE(Returns);
    EXPECT_TRUE(Returns->ReturnBlocks.empty());
    EXPECT_TRUE(Returns->ReturningCalls.empty());
  }
}

TEST(RegistrationSEHParentExit, RejectsChangedSourceAndInvocationEvidence) {
  for (unsigned Mutation = 0; Mutation != 37; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = parentExit(false);
    auto &States = *Function.RegistrationStates;
    auto &Exit = States.SEHInvocationExits.front();
    auto &Block = Function.Blocks[1];
    auto &Restore = Block.Ops.front();
    auto &Branch = Block.Ops.back();
    switch (Mutation) {
    case 0:
      States.SEHInvocationExitsComplete = false;
      break;
    case 1:
      ++Exit.Address;
      break;
    case 2:
      ++Exit.OpSeq;
      break;
    case 3:
      ++Exit.EndAddress;
      break;
    case 4:
      ++Exit.TargetVA;
      break;
    case 5:
      ++Exit.StackRestoreVA;
      break;
    case 6:
      ++Exit.StackRestoreSeq;
      break;
    case 7:
      ++Exit.StackOffset;
      break;
    case 8:
      ++Exit.EntryVA;
      break;
    case 9:
      ++Exit.CallVA;
      break;
    case 10:
      Branch.Inputs[0].ConstVal = 0x1000;
      break;
    case 11:
      Branch.Inputs[0].Size = 2;
      break;
    case 12:
      Branch.Opcode = NdOp::COND_BR;
      break;
    case 13:
      Branch.Dead = true;
      break;
    case 14:
      Restore.Dead = true;
      break;
    case 15:
      Restore.Output.RegOff = x86reg::RBP;
      break;
    case 16:
      Restore.Output.Size = 2;
      break;
    case 17:
      Restore.Opcode = NdOp::INT_ADD;
      break;
    case 18:
      Restore.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 19:
      States.FrameValues.clear();
      break;
    case 20:
      States.FrameValues.front().EstablishedFrameOffset.reset();
      break;
    case 21:
      States.SEHInvocationExits.push_back(Exit);
      break;
    case 22:
      Block.Succs.clear();
      break;
    case 23:
      Block.Succs.push_back(0);
      break;
    case 24:
      States.Blocks[2].CallbackOnly = true;
      break;
    case 25:
      States.Blocks[1].CallbackOnly = false;
      break;
    case 26:
      States.Blocks[2].Reached = false;
      break;
    case 27:
      States.RegistrationLifetimeComplete = false;
      break;
    case 28:
      Function.Blocks[0].Ops[0].Inputs[0].ConstVal += 1;
      break;
    case 29:
      Function.Blocks[0].Ops[0].ExactArguments = true;
      break;
    case 30:
      Function.ExceptionMetadata->Registration->Scopes[0].IsFinally = false;
      break;
    case 31:
      Restore.Output.TheArch = Arch::X64;
      break;
    case 32:
      Restore.Inputs[0] = MedVar::makeConst(0, 4);
      break;
    case 33:
      Block.Ops.push_back(Branch);
      break;
    case 34:
      States.Blocks.push_back(States.Blocks[1]);
      break;
    case 35:
      Function.SkippedSSA = true;
      break;
    case 36:
      States.SEHParentChainComplete = false;
      break;
    }
    EXPECT_FALSE(getRegistrationSEHParentExits(Function));
    EXPECT_FALSE(registrationSEHCallbackOwners(Function));
  }
}

TEST(RegistrationSEHParentExit, RetainsOnlyAnAuthenticatedOrdinaryCallback) {
  auto Function = parentExit(false);
  auto &Chain = *Function.ExceptionMetadata->Registration;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -1;
  Chain.Scopes = {{-1, 0x1700, 0x1750, false}, {0, 0, 0x1800, true}};
  for (auto &State : Function.RegistrationStates->Blocks)
    State.Levels = {0};
  ASSERT_TRUE(getRegistrationSEHNonlocalCalls(Function));
  EXPECT_EQ(getX86RegistrationSEHRetainedScopes(Function),
            std::optional<std::vector<uint32_t>>({1}));
  Function.Blocks[0].Ops[0].Inputs[0].ConstVal += 1;
  EXPECT_FALSE(getX86RegistrationSEHRetainedScopes(Function));
}

TEST(RegistrationSEHParentExit, OneBranchMayEndSeveralInvocations) {
  auto Function = parentExit(false);
  auto Exit = Function.RegistrationStates->SEHInvocationExits.front();
  Exit.CallVA = 0;
  Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
  auto Exits = getRegistrationSEHParentExits(Function);
  ASSERT_TRUE(Exits);
  ASSERT_EQ(Exits->at(1).Invocations.size(), 2u);
  --Function.RegistrationStates->SEHInvocationExits.back().StackOffset;
  EXPECT_FALSE(getRegistrationSEHParentExits(Function));
}

TEST(RegistrationSEHParentExit, RestoreMustDominateTheBranchInItsOwnCallback) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = parentExit(true);
    auto &States = *Function.RegistrationStates;
    MedBlock Jump = Function.Blocks[1];
    Jump.Id = 3;
    Jump.StartAddr = 0x1803;
    Jump.Ops.erase(Jump.Ops.begin());
    Function.Blocks[1].Ops.pop_back();
    Function.Blocks[1].EndAddr = 0x1803;
    Function.Blocks[1].Succs = {3};
    States.Blocks[1].Range.End = 0x1803;
    auto JumpState = States.Blocks[1];
    JumpState.BlockId = 3;
    JumpState.Range = {0x1803, 0x1805};
    States.Blocks.push_back(JumpState);
    Function.Blocks.push_back(Jump);
    if (Mutation == 1)
      Function.ExceptionMetadata->Registration->Scopes.front().HandlerVA =
          States.SEHInvocationExits.front().EntryVA = 0x1803;
    if (Mutation == 2)
      Function.Blocks[1].Succs = {1};
    if (Mutation == 3) {
      MedBlock Bypass = Function.Blocks[1];
      Bypass.Id = 4;
      Bypass.StartAddr = 0x1700;
      Bypass.EndAddr = 0x1702;
      Bypass.Ops.clear();
      Bypass.Succs = {1, 3};
      Function.Blocks.push_back(Bypass);
      auto State = States.Blocks[1];
      State.BlockId = 4;
      State.Range = {0x1700, 0x1702};
      States.Blocks.push_back(State);
      Function.ExceptionMetadata->Registration->Scopes.front().HandlerVA =
          States.SEHInvocationExits.front().EntryVA = 0x1700;
    }
    EXPECT_EQ(bool(getRegistrationSEHParentExits(Function)), Mutation == 0);
  }
}

TEST(RegistrationSEHParentExit, InputPE32BindsItsPhysicalParentExit) {
  const char *Path = std::getenv("NEVERD_SEH_PARENT_EXIT_INPUT_PE32");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_SEH_PARENT_EXIT_INPUT_PE32 to the source fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto *Export = Image->findExport("registration_entry");
  ASSERT_TRUE(Export);
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decoder, Export->Addr, "parent_exit");
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_TRUE(Low.RegistrationStates->Complete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  const auto Exits = getRegistrationSEHParentExits(Med);
  ASSERT_TRUE(Exits);
  ASSERT_FALSE(Exits->empty());
  const auto Owners = registrationSEHCallbackOwners(Med);
  ASSERT_TRUE(Owners);
  for (const auto &[Block, Exit] : *Exits) {
    EXPECT_EQ(Owners->at(Block), std::make_pair(Exit.EntryVA, true));
    EXPECT_FALSE(Owners->count(Exit.TargetBlock));
  }
}

TEST(RegistrationSEHParentExit, InputPE32RejectsAnAbandonedUnwindGuard) {
  const char *Path = std::getenv("NEVERD_SEH_UNREPAIRED_PARENT_INPUT_PE32");
  if (!Path)
    GTEST_SKIP()
        << "set NEVERD_SEH_UNREPAIRED_PARENT_INPUT_PE32 to the control";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto *Export = Image->findExport("registration_entry");
  ASSERT_TRUE(Export);
  Decoder Decoder;
  ASSERT_TRUE(Decoder.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decoder, Export->Addr, "parent_exit");
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_TRUE(Low.RegistrationStates->ChainOperationsComplete);
  EXPECT_TRUE(Low.RegistrationStates->SEHInvocationExitsComplete);
  EXPECT_FALSE(Low.RegistrationStates->SEHParentChainComplete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  EXPECT_FALSE(getRegistrationSEHParentExits(Med));
}

TEST(RegistrationSEHParentExit, FailedOutliningRestoresTheOriginalModule) {
  for (bool ExistingIntrinsic : {false, true}) {
    SCOPED_TRACE(ExistingIntrinsic);
    auto Source = parentExit(false);
    llvm::LLVMContext Context;
    llvm::Module Module("parent-rollback", Context);
    Module.setDataLayout("e-p:32:32");
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Finally = llvm::BasicBlock::Create(Context, "finally", Function);
    auto *Resume = llvm::BasicBlock::Create(Context, "resume", Function);
    llvm::IRBuilder<> Builder(Entry);
    Builder.CreateBr(Finally);
    Builder.SetInsertPoint(Finally);
    Builder.CreateBr(Resume);
    Builder.SetInsertPoint(Resume);
    Builder.CreateRetVoid();
    if (ExistingIntrinsic)
      llvm::Intrinsic::getOrInsertDeclaration(&Module,
                                              llvm::Intrinsic::eh_sjlj_longjmp);
    const std::map<int, llvm::BasicBlock *> Blocks{
        {0, Entry}, {1, Finally}, {2, Resume}};
    auto Plan = x86_registration::prepareSEHParentExits(Source, Blocks, {});
    ASSERT_TRUE(Plan);
    const auto Text = [&] {
      std::string Buffer;
      llvm::raw_string_ostream Stream(Buffer);
      Module.print(Stream, nullptr);
      return Buffer;
    };
    const auto Original = Text();
    {
      x86_registration::SEHParentExitProjection Projection(*Function,
                                                           Source.Entry, *Plan);
      EXPECT_NE(Finally->getTerminator()->getSuccessor(0), Resume);
      ASSERT_EQ(Projection.calls().size(), 1u);
      EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      Projection.rollback();
      Projection.rollback();
    }
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    EXPECT_EQ(Text(), Original);
  }
}

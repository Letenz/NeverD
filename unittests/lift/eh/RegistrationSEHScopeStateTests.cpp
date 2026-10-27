//===- RegistrationSEHScopeStateTests.cpp - Logical state addresses -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationSEHDynamicScope.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>

using namespace neverd;

namespace {

TEST(RegistrationSEHDynamicScope, ProvesUnreachedSourceAddressExpressions) {
  for (unsigned Mutation = 0; Mutation != 28; ++Mutation) {
    SCOPED_TRACE(Mutation);
    MedFunc Source;
    Source.Entry = 0x1000;
    auto &EH = Source.ExceptionMetadata.emplace();
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    auto &Chain = EH.Registration.emplace();
    Chain.TryLevelOffset = -4;
    Chain.SeededTryLevel = -1;
    Chain.Scopes = {{-1, 0, 0x1040, true}};
    auto &States = Source.RegistrationStates.emplace();
    States.Complete = States.RegistrationLifetimeComplete = true;
    States.Blocks = {{0, {0x1000, 0x1010}, {-1, 0}}, {1, {0x1020, 0x1030}, {}}};
    States.FrameValues = {{0x1000, 0, 0}};
    auto Variable = [](int Id, uint16_t Size) {
      MedVar Result;
      Result.Kind = MedVar::Temp;
      Result.Id = Id;
      Result.SSAVer = 1;
      Result.Size = Size;
      return Result;
    };
    MedBlock Entry;
    Entry.Id = 0;
    Entry.StartAddr = 0x1000;
    Entry.EndAddr = 0x1010;
    MedOp Base;
    Base.Opcode = NdOp::COPY;
    Base.Addr = 0x1000;
    Base.OriginSeq = 0;
    Base.Output = Variable(1, 4);
    Base.NumInputs = 1;
    Base.Inputs[0] = Variable(0, 4);
    Entry.Ops.push_back(Base);
    Source.Blocks.push_back(Entry);
    MedBlock Dead;
    Dead.Id = 1;
    Dead.StartAddr = 0x1020;
    Dead.EndAddr = 0x1030;
    MedOp Add;
    Add.Opcode = NdOp::INT_ADD;
    Add.Addr = 0x1020;
    Add.OriginSeq = 0;
    Add.Output = Variable(2, 4);
    Add.NumInputs = 2;
    Add.Inputs[0] = Base.Output;
    Add.Inputs[1] = MedVar::makeConst(uint32_t(-4), 4);
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Addr = 0x1020;
    Widen.OriginSeq = 1;
    Widen.Output = Variable(3, 8);
    Widen.NumInputs = 1;
    Widen.Inputs[0] = Add.Output;
    MedOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = 0x1020;
    Store.OriginSeq = 2;
    Store.NumInputs = 2;
    Store.Inputs[0] = Widen.Output;
    Store.Inputs[1] = MedVar::makeConst(UINT32_MAX, 4);
    switch (Mutation) {
    case 1:
      std::swap(Add.Inputs[0], Add.Inputs[1]);
      break;
    case 2:
      Add.Opcode = NdOp::INT_SUB;
      Add.Inputs[1] = MedVar::makeConst(4, 4);
      break;
    case 3:
      Store.Inputs[0] = Add.Output;
      break;
    case 4:
      States.FrameValues.clear();
      break;
    case 5:
      Add.Inputs[1] = MedVar::makeConst(uint32_t(-8), 4);
      break;
    case 6:
      Add.Output.Size = Add.Inputs[0].Size = Add.Inputs[1].Size = 2;
      Widen.Inputs[0] = Add.Output;
      break;
    case 7:
      Widen.Opcode = NdOp::INT_SEXT;
      break;
    case 8:
      Source.Blocks[0].Ops.push_back(Base);
      break;
    case 9:
      Add.Output.Size = Add.Inputs[0].Size = Add.Inputs[1].Size = 8;
      Widen.Inputs[0] = Add.Output;
      break;
    }
    if (Mutation >= 10) {
      MedBlock Other = Entry;
      Other.Id = 2;
      Other.StartAddr = 0x1060;
      Other.EndAddr = 0x1070;
      Other.Ops.front().Addr = 0x1060;
      Other.Ops.front().Output = Variable(5, 4);
      States.Blocks.push_back({2, {0x1060, 0x1070}, {}});
      if (Mutation != 13 && (Mutation < 21 || Mutation > 25))
        States.FrameValues.push_back({0x1060, 0, Mutation == 12 ? 4 : 0});
      if (Mutation >= 21 && Mutation <= 25) {
        auto &Root = Other.Ops.front();
        Root.OriginSeq = Mutation == 22 ? 0 : -1;
        Root.RegistrationRoot =
            MedOp::RegistrationRootKind::EstablishedFramePointer;
        Root.RegistrationStackOffset = Mutation == 23 ? 1 : 0;
        Root.Output.Kind = MedVar::Reg;
        Root.Output.TheArch = Arch::X86;
        Root.Output.RegOff = getTargetRegInfo(Arch::X86).FramePointer;
        if (Mutation == 24) {
          Root.RegistrationRoot =
              MedOp::RegistrationRootKind::CallbackStackPointer;
          Root.Output.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
        }
        Root.Inputs[0] = Root.Output;
        Root.Inputs[0].SSAVer = 0;
        if (Mutation == 25)
          Root.Inputs[0].Size = 2;
      }
      Source.Blocks.push_back(Other);
      PhiNode Phi;
      Phi.Output = Variable(4, Mutation == 20 ? 8 : 4);
      Phi.Args = {{0, Base.Output}, {2, Other.Ops.front().Output}};
      if (Mutation == 11)
        std::reverse(Phi.Args.begin(), Phi.Args.end());
      if (Mutation == 14)
        Phi.Args.back().second = MedVar::makeConst(0, 4);
      if (Mutation == 15)
        Phi.Args.back().second.Size = 2;
      if (Mutation == 17)
        Phi.Args.back().second = Phi.Output;
      if (Mutation == 18)
        Phi.Args.clear();
      if (Mutation == 26)
        Phi.ExceptionalEntry = MedVar::makeConst(0, 4);
      if (Mutation == 27)
        Phi.ExceptionalEntry = Base.Output;
      Dead.Phis.push_back(Phi);
      if (Mutation == 16)
        Dead.Phis.push_back(Phi);
      Add.Inputs[0] = Phi.Output;
      if (Mutation == 19) {
        auto Duplicate = Base;
        Duplicate.Output = Phi.Output;
        Source.Blocks.front().Ops.push_back(Duplicate);
      }
    }
    Dead.Ops = {Add, Widen, Store};
    Source.Blocks.push_back(Dead);

    llvm::LLVMContext Context;
    llvm::Module Module("source-state-address", Context);
    llvm::IRBuilder<> B(Context);
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Parent));
    auto *Slot = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64));
    auto *Address = B.CreateGEP(B.getInt8Ty(), Slot, B.getInt32(24));
    auto *Write = B.CreateStore(B.getInt32(-1), Address);
    Write->setVolatile(true);
    Write->setAlignment(llvm::Align(1));
    Write->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        llvm::MDNode::get(Context,
                          {llvm::ConstantAsMetadata::get(B.getInt64(0x1000)),
                           llvm::ConstantAsMetadata::get(B.getInt64(0x1020)),
                           llvm::ConstantAsMetadata::get(B.getInt32(2)),
                           llvm::ConstantAsMetadata::get(B.getInt32(1)),
                           llvm::ConstantAsMetadata::get(B.getInt8(1))}));
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto Bound = coff_registration::bindDynamicSEHState(Source, {Slot, 32, 0},
                                                        {Parent}, {});
    ASSERT_TRUE(bool(Bound)) << llvm::toString(Bound.takeError());
    ASSERT_TRUE(Bound->Enabled);
    EXPECT_EQ(Bound->Writes.count(Write),
              Mutation < 4 || Mutation == 10 || Mutation == 11 ||
                      Mutation == 21 || Mutation == 27
                  ? 1u
                  : 0u);
    // Static address binding grants no runtime state-transition receipt.
    EXPECT_TRUE(Bound->CallbackWrites.empty());
    EXPECT_TRUE(States.SEHStateStores.empty());
    EXPECT_TRUE(States.Blocks.back().levels().empty());
  }
}

} // namespace

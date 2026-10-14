//===- RegistrationSEHReturnTests.cpp - Finally CALL/RET contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/X86RegistrationSEHControl.h"
#include "neverd/ir/med/MedIR.h"

namespace {
using namespace neverd;

MedFunc returningFinally() {
  MedFunc Function;
  Function.ExceptionMetadata.emplace();
  Function.ExceptionMetadata->Registration.emplace();
  Function.ExceptionMetadata->Registration->Scopes.push_back(
      {-1, 0, 0x1040, true});
  Function.RegistrationStates.emplace();
  Function.Blocks.resize(3);
  auto &Call = Function.Blocks[0];
  Call.Id = 1;
  Call.StartAddr = 0x1000;
  Call.EndAddr = 0x1005;
  Call.Succs = {2};
  MedOp Invoke;
  Invoke.Opcode = NdOp::CALL;
  Invoke.Addr = 0x1000;
  Invoke.OriginSeq = 0;
  Invoke.NumInputs = 1;
  Invoke.Inputs[0] = MedVar::makeConst(0x1040, 4);
  Call.Ops.push_back(Invoke);
  auto &Continuation = Function.Blocks[1];
  Continuation.Id = 2;
  Continuation.StartAddr = 0x1005;
  Continuation.EndAddr = 0x1006;
  auto &Callback = Function.Blocks[2];
  Callback.Id = 3;
  Callback.StartAddr = 0x1040;
  Callback.EndAddr = 0x1041;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1040;
  Return.OriginSeq = 2;
  Callback.Ops.push_back(Return);
  RegistrationSEHInvocationExit Exit;
  Exit.EntryVA = 0x1040;
  Exit.CallVA = 0x1000;
  Exit.Address = 0x1040;
  Exit.EndAddress = 0x1041;
  Exit.OpSeq = 2;
  Exit.TargetVA = 0x1005;
  Exit.StackOffset = -32;
  Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
  return Function;
}

TEST(RegistrationSEHReturn, BindsPhysicalReturnToItsCallingContinuation) {
  auto Function = returningFinally();
  const auto Returns = getX86RegistrationSEHReturnBlocks(Function);
  ASSERT_TRUE(Returns);
  EXPECT_EQ(*Returns, (std::map<int, va_t>{{3, 0x1040}}));
  std::reverse(Function.Blocks.begin(), Function.Blocks.end());
  EXPECT_EQ(getX86RegistrationSEHReturnBlocks(Function), Returns);
}

TEST(RegistrationSEHReturn, RejectsChangedInvocationAndContinuationIdentities) {
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = returningFinally();
    auto &States = *Function.RegistrationStates;
    auto &Exit = States.SEHInvocationExits.front();
    switch (Mutation) {
    case 0:
      States.SEHInvocationExitsComplete = false;
      break;
    case 1:
      Exit.Exit = RegistrationSEHInvocationExit::Kind::ParentJump;
      break;
    case 2:
      Exit.CallVA = 0;
      break;
    case 3:
      Exit.EntryVA += 1;
      break;
    case 4:
      Exit.OpSeq += 1;
      break;
    case 5:
      Exit.EndAddress += 1;
      break;
    case 6:
      Exit.TargetVA += 1;
      break;
    case 7:
      Exit.StackRestoreVA = Exit.Address;
      break;
    case 8:
      Function.Blocks[0].Succs.push_back(3);
      break;
    case 9:
      Function.Blocks[2].Ops.front().Opcode = NdOp::BRANCH;
      break;
    case 10:
      Function.Blocks[0].Ops.front().Inputs[0].ConstVal += 1;
      break;
    case 11:
      States.SEHInvocationExits.push_back(Exit);
      break;
    }
    EXPECT_FALSE(getX86RegistrationSEHReturnBlocks(Function));
  }
}

TEST(RegistrationSEHReturn, OneReturnCanServeDistinctOrdinaryInvocations) {
  auto Function = returningFinally();
  MedBlock Caller = Function.Blocks.front();
  Caller.Id = 4;
  Caller.StartAddr += 0x20;
  Caller.EndAddr += 0x20;
  Caller.Ops.front().Addr += 0x20;
  Function.Blocks.push_back(Caller);
  auto Exit = Function.RegistrationStates->SEHInvocationExits.front();
  Exit.CallVA += 0x20;
  Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
  EXPECT_TRUE(getX86RegistrationSEHReturnBlocks(Function));
}

} // namespace

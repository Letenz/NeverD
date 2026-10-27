//===- RegistrationSEHRuntimeReturnTests.cpp - Finally result proof -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguishes runtime-only finally returns from observable source results.

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationCallResult.h"

#include <algorithm>

using namespace neverd;

namespace {

constexpr va_t FinallyVA = 0x401100;

MedVar result(unsigned Id, uint16_t Width = 4) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.Size = Width;
  return Value;
}

class RegistrationSEHRuntimeReturn : public testing::Test {
protected:
  MedFunc Function;
  RegistrationCallFrameEffect Call;

  void SetUp() override {
    Function.Entry = 0x401000;
    auto &EH = Function.ExceptionMetadata.emplace();
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.CodeRange = {Function.Entry, 0x401800};
    EH.Registration.emplace().Scopes = {{-1, 0, FinallyVA, true}};
    auto &States = Function.RegistrationStates.emplace();
    States.Complete = States.CallbackStatesComplete = true;
    States.RegistrationLifetimeComplete = true;
    RegistrationBlockState State;
    State.BlockId = 20;
    State.Range = {FinallyVA, FinallyVA + 16};
    State.ExplicitLevels = {0};
    State.CallbackOnly = State.CanDispatch = State.Reached = true;
    States.Blocks.push_back(State);
    States.SEHControls = {{RegistrationSEHControlContract::Kind::NearReturn,
                           FinallyVA + 15, FinallyVA + 16}};
    Function.Blocks.resize(2);
    auto &Parent = Function.Blocks[0];
    Parent.Id = 10;
    Parent.StartAddr = Function.Entry;
    Parent.EndAddr = Function.Entry + 16;
    auto &Body = Function.Blocks[1];
    Body.Id = 20;
    Body.StartAddr = FinallyVA;
    Body.EndAddr = FinallyVA + 16;
    MedOp Invoke;
    Invoke.Opcode = NdOp::INDIR_CALL;
    Invoke.Addr = Call.Address = FinallyVA;
    Invoke.OriginSeq = Call.OpSeq = 7;
    Call.Target = 0x403000;
    Invoke.addInput(MedVar::makeConst(Call.Target, 4));
    Invoke.Output = result(1);
    Body.Ops.push_back(Invoke);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = FinallyVA + 15;
    Return.OriginSeq = 8;
    Return.addInput(result(1));
    Body.Ops.push_back(Return);
  }

  bool unused(va_t Entry = FinallyVA) {
    size_t Work = 0;
    return hasUnusedX86RegistrationCallResult(Function, Call, Work, true,
                                              Entry);
  }
};

TEST_F(RegistrationSEHRuntimeReturn, OnlyDiscardsRuntimeFinallyReturnValues) {
  EXPECT_FALSE(unused(0));
  EXPECT_TRUE(unused());
  Function.ExceptionMetadata->Encoding = ExceptionEncoding::X86ScopeTableEH4;
  EXPECT_TRUE(unused());
  std::reverse(Function.Blocks.begin(), Function.Blocks.end());
  EXPECT_TRUE(unused());
}

TEST_F(RegistrationSEHRuntimeReturn, FollowsOnlyUnobservedScalarTransports) {
  auto &Body = Function.Blocks[1];
  MedOp Extend;
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.addInput(result(1));
  Extend.Output = result(2, 8);
  Body.Ops.insert(Body.Ops.begin() + 1, Extend);
  Body.Ops.back().Inputs[0] = Extend.Output;
  EXPECT_TRUE(unused());
  Body.Ops[1].Opcode = NdOp::INT_ADD;
  EXPECT_FALSE(unused());
}

TEST_F(RegistrationSEHRuntimeReturn, RequiresCompleteSourceAndControlProofs) {
  auto Original = Function;
  for (unsigned Edit = 0; Edit != 13; ++Edit) {
    SCOPED_TRACE(Edit);
    Function = Original;
    auto &State = *Function.RegistrationStates;
    switch (Edit) {
    case 0:
      State.Complete = false;
      break;
    case 1:
      State.CallbackStatesComplete = false;
      break;
    case 2:
      State.RegistrationLifetimeComplete = false;
      break;
    case 3:
      State.SEHInvocationExitsComplete = false;
      break;
    case 4:
      State.Blocks[0].Unknown = true;
      break;
    case 5:
      State.Blocks[0].CallbackOnly = false;
      break;
    case 6:
      State.Blocks[0].CanDispatch = false;
      break;
    case 7:
      State.Blocks[0].Reached = false;
      break;
    case 8:
      ++State.Blocks[0].Range.End;
      break;
    case 9:
      State.SEHControls.clear();
      break;
    case 10:
      ++State.SEHControls[0].EndAddress;
      break;
    case 11:
      State.SEHControls.push_back(State.SEHControls[0]);
      break;
    case 12:
      Function.Blocks[1].Ops.back().OriginSeq = -1;
      break;
    }
    EXPECT_FALSE(unused());
  }
}

TEST_F(RegistrationSEHRuntimeReturn, RejectsOrdinaryAndSharedEntries) {
  auto Original = Function;
  for (unsigned Edit = 0; Edit != 6; ++Edit) {
    SCOPED_TRACE(Edit);
    Function = Original;
    switch (Edit) {
    case 0:
      Function.Blocks[0].Succs = {20};
      break;
    case 1: {
      auto Ordinary = Function.Blocks[1].Ops.front();
      Ordinary.Opcode = NdOp::CALL;
      Ordinary.Addr = Function.Entry;
      Ordinary.Inputs[0] = MedVar::makeConst(FinallyVA, 4);
      Ordinary.Output = result(3);
      Function.Blocks[0].Ops.push_back(Ordinary);
      break;
    }
    case 2:
      Function.ExceptionMetadata->Registration->Scopes.push_back(
          {-1, FinallyVA, 0x401500, false});
      break;
    case 3:
      Function.ExceptionMetadata->Registration->Scopes.push_back(
          {-1, 0x401500, FinallyVA + 4, false});
      break;
    case 4:
      Function.Blocks[1].Succs = {10};
      break;
    case 5: {
      RegistrationSEHInvocationExit Exit;
      Exit.EntryVA = FinallyVA;
      Exit.CallVA = Function.Entry;
      Function.RegistrationStates->SEHInvocationExits.push_back(Exit);
      break;
    }
    }
    EXPECT_FALSE(unused());
  }
}

TEST_F(RegistrationSEHRuntimeReturn, KeepsParentReturnsAndOtherUsesObservable) {
  Function.Blocks[0].Ops.push_back(Function.Blocks[1].Ops.back());
  Function.Blocks[0].Ops.back().Addr = Function.Entry + 15;
  EXPECT_FALSE(unused());
  Function.Blocks[0].Ops.clear();
  MedOp Observe;
  Observe.Opcode = NdOp::STORE;
  Observe.addInput(MedVar::makeConst(0x403004, 4));
  Observe.addInput(result(1));
  Function.Blocks[1].Ops.insert(Function.Blocks[1].Ops.begin() + 1, Observe);
  EXPECT_FALSE(unused());
}

} // namespace

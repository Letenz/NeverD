//===- RegistrationStateTests.cpp - Registration-state CFG proofs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace {

using namespace neverd;

void addSlotStore(LowBlock &Block, int32_t Value, uint16_t Width = 4) {
  LowOp Address;
  Address.Opcode = NdOp::INT_ADD;
  Address.Output = NdVar::tmp(0, 4);
  Address.addInput(NdVar::reg(x86reg::RBP, 4));
  Address.addInput(NdVar::cst(uint32_t(-4), 4));
  Address.Addr = Block.StartAddr;
  Block.Ops.push_back(Address);
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Address.Output);
  Store.addInput(NdVar::cst(uint32_t(Value), Width));
  Store.Addr = Block.StartAddr;
  Block.Ops.push_back(Store);
}

LowFunc makeBranchingFrame() {
  LowFunc F;
  F.Entry = 0x1000;
  F.ExceptionMetadata.emplace();
  F.ExceptionMetadata->CodeRange = {0x1000, 0x2000};
  RegistrationChainInfo &Chain = F.ExceptionMetadata->Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = 0x1000;
  Chain.Scopes.push_back({-1, 0x1800, 0x1900, false});
  Chain.TryLevelStores = {{0x1010, 0x1017, 0}, {0x1020, 0x1027, -1}};
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    LowBlock &B = F.Blocks[I];
    B.Id = I;
    B.StartAddr = 0x1000 + I * 0x10;
    B.EndAddr = B.StartAddr + 7;
    B.InstructionBoundaries.push_back({B.StartAddr, 7});
  }
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Succs = {3};
  LowOp Install;
  Install.Opcode = NdOp::STORE;
  Install.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Install.addInput(NdVar::cst(0, 8));
  Install.addInput(NdVar::reg(x86reg::RSP, 4));
  Install.Addr = 0x1000;
  F.Blocks[0].Ops.push_back(Install);
  addSlotStore(F.Blocks[1], 0);
  addSlotStore(F.Blocks[2], -1);
  return F;
}

TEST(RegistrationState, RetainsBothStatesAtJoin) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 4u);
  EXPECT_EQ(Result.Blocks[3].Levels, (std::vector<int32_t>{-1, 0}));
  EXPECT_FALSE(registrationRangesWhere(
      Result, [](int32_t Level) { return Level == 0; }));
}

TEST(RegistrationState, ReplaysLoopBackedges) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[3].Succs = {1};
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[0].Levels.empty());
  EXPECT_EQ(Result.Blocks[1].Levels, (std::vector<int32_t>{-1, 0}));
}

TEST(RegistrationState, DoesNotSeedBeforeTheRegistrationIsInstalled) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[0].Levels.empty());
  EXPECT_EQ(Result.Blocks[1].Levels, (std::vector<int32_t>{-1}));
}

TEST(RegistrationState, MissingInstallationBoundaryCannotAuthorizeTheSeed) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->ChainInstallVA++;
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, DoesNotApplyStoresFromUnreachableBlocks) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[2].Levels.empty());
  EXPECT_EQ(Result.Blocks[3].Levels, (std::vector<int32_t>{0}));
}

TEST(RegistrationState, KeepsTheIncomingLevelUntilTheStoreRetires) {
  LowFunc F = makeBranchingFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_EQ(Result.Blocks[1].Levels, (std::vector<int32_t>{-1}));
}

TEST(RegistrationState, InvalidInstructionBoundaryCannotAuthorizeAState) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->TryLevelStores[0].StoreVA++;
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 4u);
  EXPECT_TRUE(Result.Blocks[3].Unknown);
}

TEST(RegistrationState, InvalidStateCannotDisappearAtALaterReset) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->TryLevelStores[0].Level = 9;
  F.ExceptionMetadata->Registration->TryLevelStores.push_back(
      {0x1030, 0x1037, -1});
  addSlotStore(F.Blocks[3], -1);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(registrationRangesWhere(
      Result, [](int32_t Level) { return Level >= 0; }));
}

TEST(RegistrationState, APartialWriteCannotKeepThePreviousState) {
  LowFunc F = makeBranchingFrame();
  addSlotStore(F.Blocks[3], 1, 1);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, ADynamicWriteCannotKeepThePreviousState) {
  LowFunc F = makeBranchingFrame();
  addSlotStore(F.Blocks[3], 1);
  F.Blocks[3].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, LiftedStoreMustAgreeWithTheScanner) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, MissingSlotDoesNotInventScopeRanges) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Registration->TryLevelOffset.reset();
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
  EXPECT_FALSE(Result.Diagnostics.empty());
}

TEST(RegistrationState, MissingSuccessorFailsTheWholeProof) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[1].Succs.push_back(99);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, CatchEntryUsesTheRuntimeCatchState) {
  LowFunc F = makeBranchingFrame();
  CxxExceptionInfo &Cxx = F.ExceptionMetadata->Cxx.emplace();
  Cxx.MaxState = 2;
  Cxx.UnwindMap.resize(2);
  Cxx.UnwindMap[0].ToState = -1;
  Cxx.UnwindMap[1].ToState = -1;
  CxxTryBlock Try;
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = 0x1040;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(Try);
  F.Blocks.push_back(LowBlock{});
  F.Blocks.back().Id = 4;
  F.Blocks.back().StartAddr = 0x1040;
  F.Blocks.back().EndAddr = 0x1041;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 5u);
  EXPECT_EQ(Result.Blocks.back().Levels, (std::vector<int32_t>{1}));
}

} // namespace

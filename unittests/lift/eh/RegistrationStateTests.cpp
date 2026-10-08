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

void emitOp(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs,
            NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  Op.MemoryAddressSpace = Space;
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  Block.Ops.push_back(Op);
}

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
  F.ExceptionMetadata->Personality = ExceptionPersonality::ExceptHandler3;
  F.ExceptionMetadata->Encoding = ExceptionEncoding::X86ScopeTableEH3;
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
  auto Emit = [&](NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs,
                  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Output = Output;
    Op.Addr = 0x1000;
    Op.MemoryAddressSpace = Space;
    for (NdVar Input : Inputs)
      Op.addInput(Input);
    F.Blocks[0].Ops.push_back(Op);
  };
  Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
       {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  Emit(NdOp::COPY, NdVar::reg(x86reg::RBP, 4), {NdVar::reg(x86reg::RSP, 4)});
  Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
       {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  Emit(NdOp::LOAD, NdVar::tmp(8, 4), {NdVar::cst(0, 8)},
       NdMemoryAddressSpace::X86FS);
  Emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(8, 4)});
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

TEST(RegistrationState, InstallationBytesNeedTheActualLiftedChainWrite) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Ops.pop_back();
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.empty());
}

TEST(RegistrationState, AnUnknownHandlerDoesNotAcquireSEHSemantics) {
  LowFunc F = makeBranchingFrame();
  F.ExceptionMetadata->Personality = ExceptionPersonality::Unknown;
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
  F.ExceptionMetadata->Personality = ExceptionPersonality::CxxFrameHandlerX86;
  F.ExceptionMetadata->Encoding = ExceptionEncoding::X86CxxFuncInfo;
  F.ExceptionMetadata->Registration->Scopes.clear();
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

TEST(RegistrationState, PreservesFrameAliasesAcrossInstructionsAndCFGEdges) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_TRUE(Result.Blocks.back().Unknown);
}

TEST(RegistrationState, PreservesPossibleAliasesWhenOnlyOneBranchDefinesThem) {
  LowFunc F = makeBranchingFrame();
  emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, PreservesAliasesSpilledIntoKnownFrameCells) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(8, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, PartiallyOverwrittenSpillsKeepPossibleFrameAliases) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::tmp(0, 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::cst(0, 1)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(8, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::reg(x86reg::RDI, 4), NdVar::cst(1, 1)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, ReassignedEBPDoesNotKeepTheOriginalSlotIdentity) {
  LowFunc F = makeBranchingFrame();
  LowOp Change;
  Change.Addr = 0x1010;
  Change.Opcode = NdOp::COPY;
  Change.Output = NdVar::reg(x86reg::RBP, 4);
  Change.addInput(NdVar::cst(0x800000, 4));
  F.Blocks[1].Ops.insert(F.Blocks[1].Ops.begin(), Change);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
}

TEST(RegistrationState, UnlinkEndsTheLiveRegistration) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(8, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::cst(0, 8), NdVar::reg(x86reg::RCX, 4)},
         NdMemoryAddressSpace::X86FS);
  F.Blocks[3].Succs = {4};
  LowBlock Exit;
  Exit.Id = 4;
  Exit.StartAddr = 0x1040;
  Exit.EndAddr = 0x1041;
  F.Blocks.push_back(Exit);
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_TRUE(Result.Blocks.back().Levels.empty());
  EXPECT_FALSE(Result.Blocks.back().CanDispatch);
}

TEST(RegistrationState, AnUnknownChainHeadWriteCannotKeepTheOldScope) {
  LowFunc F = makeBranchingFrame();
  emitOp(F.Blocks[3], 0x1030, NdOp::STORE, {},
         {NdVar::cst(0, 8), NdVar::reg(x86reg::RCX, 4)},
         NdMemoryAddressSpace::X86FS);
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
}

TEST(RegistrationState, FinallyCanDispatchToItsEnclosingScope) {
  LowFunc F = makeBranchingFrame();
  F.Blocks[0].Succs = {1};
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes.push_back({0, 0, 0x1040, true});
  Chain.TryLevelStores.front().Level = 1;
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(1, 4);
  LowBlock Finally;
  Finally.Id = 4;
  Finally.StartAddr = 0x1040;
  Finally.EndAddr = 0x1041;
  F.Blocks.push_back(Finally);
  LowBlock Filter;
  Filter.Id = 5;
  Filter.StartAddr = 0x1800;
  Filter.EndAddr = 0x1801;
  F.Blocks.push_back(Filter);
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Blocks[4].CallbackOnly);
  EXPECT_TRUE(Result.Blocks[4].CanDispatch);
  EXPECT_EQ(Result.Blocks[4].Levels, (std::vector<int32_t>{0}));
  EXPECT_TRUE(Result.Blocks[5].CallbackOnly);
  EXPECT_FALSE(Result.Blocks[5].CanDispatch);
}

} // namespace

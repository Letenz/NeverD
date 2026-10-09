//===- RegistrationStateTests.cpp - Registration-state CFG proofs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

#include <tuple>

namespace {

using namespace neverd;

void emitOp(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs,
            NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Seq = Block.Ops.size();
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
    Op.Seq = F.Blocks[0].Ops.size();
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
  Install.Seq = F.Blocks[0].Ops.size();
  Install.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Install.addInput(NdVar::cst(0, 8));
  Install.addInput(NdVar::reg(x86reg::RSP, 4));
  Install.Addr = 0x1000;
  F.Blocks[0].Ops.push_back(Install);
  addSlotStore(F.Blocks[1], 0);
  addSlotStore(F.Blocks[2], -1);
  return F;
}

LowFunc makeCookieFrame(bool GS = false) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.ScopeTableVA = 0x3000;
  Chain.EHCookieOffset = -28;
  Chain.GSCookieOffset = GS ? -36 : -2;
  Chain.GSCookieXOROffset = GS ? 4 : 0;
  Chain.HasSecurityCookies = GS;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(uint32_t(-2), 4);
  auto &Entry = F.Blocks.front();
  auto Install = Entry.Ops.back();
  Entry.Ops.pop_back();
  emitOp(Entry, 0x1000, NdOp::COPY, NdVar::tmp(20, 4),
         {NdVar::reg(x86reg::RSP, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(24, 4)});
  emitOp(Entry, 0x1000, NdOp::LOAD, NdVar::tmp(21, 4), {NdVar::cst(0x4000, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(22, 4),
         {NdVar::tmp(21, 4), NdVar::cst(0x3000, 4)});
  auto Store = [&](int32_t Slot, NdVar Value) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(24, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Slot), 4)});
    emitOp(Entry, 0x1000, NdOp::STORE, {}, {NdVar::tmp(24, 4), Value});
  };
  Store(-8, NdVar::tmp(22, 4));
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(23, 4),
         {NdVar::tmp(21, 4), NdVar::reg(x86reg::RBP, 4)});
  Store(-28, NdVar::tmp(23, 4));
  if (GS) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(25, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(4, 4)});
    emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(26, 4),
           {NdVar::tmp(21, 4), NdVar::tmp(25, 4)});
    Store(-36, NdVar::tmp(26, 4));
  }
  Install.Seq = Entry.Ops.size();
  Install.Inputs[1] = NdVar::tmp(20, 4);
  Entry.Ops.push_back(Install);
  auto &Exit = F.Blocks.back();
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(30, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::tmp(31, 4),
         {NdVar::tmp(30, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
         {NdVar::cst(0, 4), NdVar::tmp(31, 4)}, NdMemoryAddressSpace::X86FS);
  return F;
}

TEST(RegistrationState, EH4CookiesUseTheAuthenticatedImageAndRuntimeFrame) {
  for (bool GS : {false, true}) {
    auto F = makeCookieFrame(GS);
    auto Result = analyzeRegistrationStates(F, 0x4000);
    ASSERT_TRUE(Result.Complete);
    ASSERT_TRUE(Result.RegistrationLifetimeComplete);
    EXPECT_TRUE(Result.SecurityCookiesComplete);
    EXPECT_EQ(Result.SecurityCookieVA, 0x4000u);
    EXPECT_FALSE(analyzeRegistrationStates(F).SecurityCookiesComplete);
    EXPECT_FALSE(analyzeRegistrationStates(F, 0x4004).SecurityCookiesComplete);
  }
}

LowFunc makeCheckedCookieExit() {
  auto F = makeCookieFrame(true);
  auto &Exit = F.Blocks.back();
  auto Unlink = std::move(Exit.Ops);
  Exit.Ops.clear();
  Exit.EndAddr = Exit.StartAddr + 12;
  Exit.InstructionBoundaries = {{Exit.StartAddr, 5}, {Exit.StartAddr + 5, 7}};
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(40, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-36), 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
         {NdVar::tmp(40, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(41, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(4, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::INT_XOR, NdVar::reg(x86reg::RCX, 4),
         {NdVar::reg(x86reg::RCX, 4), NdVar::tmp(41, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x5000, 4)});
  for (auto Op : Unlink) {
    Op.Addr += 5;
    Op.Seq = Exit.Ops.size();
    Exit.Ops.push_back(Op);
  }
  return F;
}

TEST(RegistrationState, EH4ExitCheckRequiresTheExactDecodedCookie) {
  auto F = makeCheckedCookieExit();
  auto Result = analyzeRegistrationStates(F, 0x4000, 0x5000);
  ASSERT_TRUE(Result.SecurityCookiesComplete);
  ASSERT_EQ(Result.CookieChecks.size(), 1u);
  EXPECT_EQ(Result.CookieCheckVA, 0x5000u);
  EXPECT_NE(Result.cookieCheck(0x1030, 4), nullptr);
  EXPECT_EQ(Result.cookieCheck(0x1030, 3), nullptr);
  EXPECT_EQ(Result.CookieChecks.front().EndAddress, 0x1035u);
  EXPECT_FALSE(analyzeRegistrationStates(F, 0x4000).SecurityCookiesComplete);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto Changed = F;
    auto &Exit = Changed.Blocks.back();
    if (Mutation == 0)
      Exit.Ops[2].Inputs[1] = NdVar::cst(8, 4);
    if (Mutation == 1)
      Exit.Ops[1].Output.Size = 1;
    if (Mutation == 2)
      Exit.Ops[3].Opcode = NdOp::INT_ADD;
    if (Mutation == 3)
      Exit.Ops[4].Inputs[0] = NdVar::cst(0x5010, 4);
    if (Mutation == 4)
      Exit.Ops[4].Seq = -1;
    if (Mutation == 5)
      Exit.InstructionBoundaries.erase(Exit.InstructionBoundaries.begin());
    if (Mutation == 6)
      Exit.Ops[4].Inputs[0].Size = 1;
    if (Mutation == 7)
      Exit.Ops[3].Output = NdVar::reg(x86reg::RDX, 4);
    auto Invalid = analyzeRegistrationStates(Changed, 0x4000, 0x5000);
    EXPECT_FALSE(Invalid.SecurityCookiesComplete) << Mutation;
    EXPECT_TRUE(Invalid.CookieChecks.empty()) << Mutation;
  }
}

TEST(RegistrationState, EH4CookieInitializationCannotBeNarrowOrUnencoded) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto F = makeCookieFrame();
    auto &Ops = F.Blocks.front().Ops;
    if (Mutation == 0)
      F.ExceptionMetadata->Registration->EHCookieXOROffset = 4;
    if (Mutation == 1)
      F.ExceptionMetadata->Registration->EHCookieOffset = -20;
    for (auto &Op : Ops) {
      if (Mutation == 2 && Op.Output == NdVar::tmp(22, 4))
        Op.Inputs[1] = NdVar::cst(0x3004, 4);
      if (Mutation == 3 && Op.Output == NdVar::tmp(21, 4))
        Op.Output.Size = 1;
      if (Mutation == 4 && Op.Opcode == NdOp::STORE && Op.NumInputs == 2 &&
          Op.Inputs[1] == NdVar::tmp(23, 4))
        Op.Inputs[1].Size = 1;
    }
    EXPECT_FALSE(analyzeRegistrationStates(F, 0x4000).SecurityCookiesComplete)
        << Mutation;
  }
}

TEST(RegistrationState, EH4CookiesAndScopeEncodingRemainRuntimePrivate) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto F = makeCookieFrame();
    auto &Block = F.Blocks[1];
    NdVar Address = NdVar::cst(Mutation == 3 ? 0x4000 : 0x3008, 4);
    if (Mutation < 3) {
      emitOp(Block, Block.StartAddr, NdOp::INT_ADD, NdVar::tmp(40, 4),
             {NdVar::reg(x86reg::RBP, 4),
              NdVar::cst(uint32_t(Mutation == 0 ? -28 : -8), 4)});
      Address = NdVar::tmp(40, 4);
    }
    if (Mutation == 0)
      emitOp(Block, Block.StartAddr, NdOp::LOAD, NdVar::tmp(41, 4), {Address});
    else
      emitOp(Block, Block.StartAddr, NdOp::STORE, {},
             {Address, NdVar::cst(0, 4)});
    EXPECT_FALSE(analyzeRegistrationStates(F, 0x4000).SecurityCookiesComplete)
        << Mutation;
  }
}

TEST(RegistrationState, IncomingCallerFrameUsesTheFinalCFGValue) {
  for (bool Agree : {false, true}) {
    LowFunc F = makeBranchingFrame();
    for (unsigned I = 1; I != 3; ++I)
      emitOp(F.Blocks[I], F.Blocks[I].StartAddr, NdOp::INT_ADD,
             NdVar::reg(x86reg::RAX, 4),
             {NdVar::reg(x86reg::RBP, 4),
              NdVar::cst(I == 1 || Agree ? 8 : 12, 4)});
    emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::tmp(9, 4),
           {NdVar::reg(x86reg::RAX, 4)});
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    EXPECT_EQ(Result.IncomingFrameAccessesComplete, Agree);
    if (Agree) {
      ASSERT_EQ(Result.IncomingFrameAccesses.size(), 1u);
      EXPECT_EQ(Result.IncomingFrameAccesses.front().Offset, 8);
      EXPECT_EQ(Result.IncomingFrameAccesses.front().Width, 4u);
      EXPECT_FALSE(Result.IncomingFrameAccesses.front().Write);
    } else
      EXPECT_TRUE(Result.IncomingFrameAccesses.empty());
  }
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

LowFunc makeNarrowCxxStateFrame(uint8_t Width, int32_t First, int32_t Other,
                                int32_t Immediate) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  auto &Chain = *EH.Registration;
  Chain.RegistrationOffset = -12;
  Chain.Scopes.clear();
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 258;
  Cxx.UnwindMap.resize(Cxx.MaxState);
  for (auto &Action : Cxx.UnwindMap)
    Action.Kind = CxxUnwindAction::ActionKind::None;
  F.Blocks[0].Ops[2].Inputs[1] = NdVar::cst(12, 4);
  F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(uint32_t(First), 4);
  F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(uint32_t(Other), 4);
  Chain.TryLevelStores[0].Level = First;
  Chain.TryLevelStores[1].Level = Other;
  auto &Write = F.Blocks[3];
  Write.EndAddr = Write.StartAddr + 4;
  Write.InstructionBoundaries = {{Write.StartAddr, 4}};
  addSlotStore(Write, Immediate, Width);
  Chain.TryLevelStores.push_back(
      {Write.StartAddr, Write.EndAddr, Immediate, Width});
  Write.Succs = {4};
  LowBlock After;
  After.Id = 4;
  After.StartAddr = 0x1040;
  After.EndAddr = 0x1041;
  F.Blocks.push_back(std::move(After));
  return F;
}

TEST(RegistrationState, NarrowCxxStoresPreserveEveryReachingHighByte) {
  for (const auto &[Width, First, Other, Immediate, Expected] :
       {std::tuple{uint8_t(1), 256, 256, 1, std::vector<int32_t>{257}},
        std::tuple{uint8_t(1), 0, 256, 1, std::vector<int32_t>{1, 257}},
        std::tuple{uint8_t(1), -1, -1, 255, std::vector<int32_t>{-1}},
        std::tuple{uint8_t(2), 0, 256, 257, std::vector<int32_t>{257}}}) {
    auto F = makeNarrowCxxStateFrame(Width, First, Other, Immediate);
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete) << unsigned(Width) << ' ' << First;
    ASSERT_EQ(Result.Blocks.size(), 5u);
    EXPECT_EQ(Result.Blocks.back().Levels, Expected);
    EXPECT_FALSE(Result.Blocks.back().Unknown);
  }
}

TEST(RegistrationState, NarrowCxxStoresNeedThePriorWholeStateAndExactWidth) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto F = makeNarrowCxxStateFrame(1, 0, 0, 1);
    auto &Store = F.ExceptionMetadata->Registration->TryLevelStores.back();
    if (Mutation == 0) {
      F.Blocks[1].Ops.back().Inputs[1] = NdVar::cst(uint32_t(-1), 4);
      F.ExceptionMetadata->Registration->TryLevelStores[0].Level = -1;
    }
    if (Mutation == 1)
      Store.Width = 4;
    if (Mutation == 2)
      F.Blocks[3].Ops.back().Inputs[1] = NdVar::cst(2, 1);
    if (Mutation == 3) {
      Store.Level = 256;
      F.Blocks[3].Ops.back().Inputs[1] = NdVar::cst(256, 1);
    }
    if (Mutation == 4)
      F.Blocks[3].Ops.front().Inputs[1] = NdVar::cst(uint32_t(-3), 4);
    if (Mutation == 5) {
      F.Blocks[1].Ops.back().Inputs[1] = NdVar::reg(x86reg::RAX, 4);
    }
    const auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete) << Mutation;
    ASSERT_EQ(Result.Blocks.size(), 5u);
    EXPECT_TRUE(Result.Blocks.back().Unknown) << Mutation;
  }
}

LowFunc makeCxxCatchContinuation(bool IncludeResume = true) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  auto &Chain = *EH.Registration;
  Chain.RegistrationOffset = -12;
  Chain.Scopes.clear();
  Chain.TryLevelStores = {
      {0x1016, 0x101d, 0}, {0x1020, 0x1027, -1}, {0x1900, 0x1907, -1}};
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0, CxxUnwindAction::ActionKind::None},
                   {-1, 0, CxxUnwindAction::ActionKind::None}};
  CxxTryBlock Try;
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = 0x1800;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(Try);

  F.Blocks.resize(IncludeResume ? 7 : 6);
  F.Blocks[0].Ops[2].Inputs[1] = NdVar::cst(12, 4);
  F.Blocks[0].Succs = {1};
  for (size_t I = 1; I < F.Blocks.size(); ++I) {
    F.Blocks[I] = LowBlock{};
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 0x10;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 7;
  }
  auto &Body = F.Blocks[1];
  Body.EndAddr = 0x101d;
  Body.InstructionBoundaries = {{0x1010, 3}, {0x1013, 3}, {0x1016, 7}};
  Body.Succs = {2};
  emitOp(Body, 0x1010, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  emitOp(Body, 0x1013, NdOp::INT_ADD, NdVar::tmp(40, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Body, 0x1013, NdOp::STORE, {},
         {NdVar::tmp(40, 4), NdVar::reg(x86reg::RSP, 4)});
  LowBlock StateStore;
  StateStore.StartAddr = 0x1016;
  addSlotStore(StateStore, 0);
  Body.Ops.insert(Body.Ops.end(), StateStore.Ops.begin(), StateStore.Ops.end());

  F.Blocks[2].InstructionBoundaries = {{0x1020, 7}};
  F.Blocks[2].Succs = {3};
  addSlotStore(F.Blocks[2], -1);
  auto &Unlink = F.Blocks[3];
  Unlink.InstructionBoundaries = {{0x1030, 7}};
  Unlink.Succs = {4};
  emitOp(Unlink, 0x1030, NdOp::INT_ADD, NdVar::tmp(50, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-12), 4)});
  emitOp(Unlink, 0x1030, NdOp::LOAD, NdVar::tmp(51, 4), {NdVar::tmp(50, 4)});
  emitOp(Unlink, 0x1030, NdOp::STORE, {}, {NdVar::cst(0, 4), NdVar::tmp(51, 4)},
         NdMemoryAddressSpace::X86FS);
  auto AddReturn = [&](LowBlock &Block, va_t Address) {
    LowInstructionBoundary Return;
    Return.Address = Address;
    Return.Size = 1;
    Return.Control = LowInstructionControl::Return;
    Block.InstructionBoundaries.push_back(Return);
    Block.EndAddr = Address + 1;
    emitOp(Block, Address, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 4)});
  };
  AddReturn(F.Blocks[4], 0x1040);
  auto &Handler = F.Blocks[5];
  Handler.StartAddr = 0x1800;
  Handler.InstructionBoundaries = {{0x1800, 5}};
  emitOp(Handler, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x1900, 4)});
  AddReturn(Handler, 0x1805);
  if (IncludeResume) {
    auto &Resume = F.Blocks[6];
    Resume.StartAddr = 0x1900;
    Resume.EndAddr = 0x1907;
    Resume.InstructionBoundaries = {{0x1900, 7}};
    Resume.Succs = {3};
    addSlotStore(Resume, -1);
  }
  return F;
}

TEST(RegistrationState, CxxCatchResumesWithTheRuntimeStackAndState) {
  const auto F = makeCxxCatchContinuation();
  const auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.CallbackStatesComplete);
  EXPECT_TRUE(Result.CxxContinuationsComplete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_TRUE(Result.ChainOperationsComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 1u);
  EXPECT_EQ(
      Result.CxxContinuations[0],
      (RegistrationCxxContinuation{0, 0, 0x1805, 0x1806, 1, 0x1900, -28}));
  EXPECT_TRUE(Result.Blocks[5].CallbackOnly);
  EXPECT_EQ(Result.Blocks[5].CxxMinimumTryLevel, 1);
  EXPECT_FALSE(Result.Blocks[6].CallbackOnly);
  EXPECT_EQ(Result.Blocks[6].Levels, (std::vector<int32_t>{1}));
}

TEST(RegistrationState,
     MissingCxxContinuationKeepsTheCandidateWithoutAuthority) {
  const auto Result =
      analyzeRegistrationStates(makeCxxCatchContinuation(false));
  ASSERT_EQ(Result.CxxContinuations.size(), 1u);
  EXPECT_EQ(Result.CxxContinuations[0].TargetVA, 0x1900u);
  EXPECT_FALSE(Result.CxxContinuationsComplete);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, CxxReturnsNeedExactContextStackAndDecodedReturn) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    auto F = makeCxxCatchContinuation();
    auto &Handler = F.Blocks[5];
    if (Mutation == 0)
      Handler.Ops[0].Inputs[0] = NdVar::cst(9, 4);
    if (Mutation == 1)
      Handler.InstructionBoundaries.back().Control =
          LowInstructionControl::TailCall;
    if (Mutation == 2)
      Handler.InstructionBoundaries.back().Immediate = 4;
    if (Mutation == 3)
      F.Blocks[1].Ops[2].Inputs[1] = NdVar::cst(0, 4);
    if (Mutation == 4)
      F.OrdinaryModuleAnalysisRoots.insert(Handler.StartAddr);
    if (Mutation == 5)
      Handler.Ops.back().Inputs[0].Size = 2;
    if (Mutation == 6)
      Handler.Ops[0].Opcode = NdOp::CALL;
    if (Mutation == 7)
      Handler.Ops.back().Seq = -1;
    const auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete) << Mutation;
    EXPECT_FALSE(Result.CxxContinuationsComplete) << Mutation;
    EXPECT_FALSE(Result.RegistrationLifetimeComplete) << Mutation;
    EXPECT_TRUE(Result.CxxContinuations.empty()) << Mutation;
  }
}

TEST(RegistrationState, NestedCxxCatchResumesTheEnclosingCatchContext) {
  auto F = makeCxxCatchContinuation();
  auto &EH = *F.ExceptionMetadata;
  auto &Cxx = *EH.Cxx;
  Cxx.MaxState = 4;
  Cxx.UnwindMap.resize(4, {-1, 0, CxxUnwindAction::ActionKind::None});
  Cxx.TryBlocks[0].CatchHigh = 3;
  auto Inner = Cxx.TryBlocks[0];
  Inner.TryLow = Inner.TryHigh = 2;
  Inner.CatchHigh = 3;
  Inner.Handlers[0].HandlerVA = 0x1850;
  Cxx.TryBlocks.push_back(Inner);
  F.Blocks.resize(12);
  F.Blocks[8] = F.Blocks[5];
  F.Blocks[8].Id = 8;
  F.Blocks[8].StartAddr = 0x1830;
  F.Blocks[8].EndAddr = 0x1836;
  for (auto &Op : F.Blocks[8].Ops)
    Op.Addr += 0x30;
  for (auto &Boundary : F.Blocks[8].InstructionBoundaries)
    Boundary.Address += 0x30;
  F.Blocks[9] = F.Blocks[8];
  F.Blocks[9].Id = 9;
  F.Blocks[9].StartAddr = 0x1850;
  F.Blocks[9].EndAddr = 0x1856;
  for (auto &Op : F.Blocks[9].Ops)
    Op.Addr += 0x20;
  for (auto &Boundary : F.Blocks[9].InstructionBoundaries)
    Boundary.Address += 0x20;
  F.Blocks[9].Ops[0].Inputs[0] = NdVar::cst(0x1820, 4);

  auto StateBlock = [&](unsigned Id, va_t Address, int32_t Level,
                        int Successor) {
    auto &Block = F.Blocks[Id];
    Block = LowBlock{};
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 7;
    Block.InstructionBoundaries = {{Address, 7}};
    Block.Succs = {Successor};
    addSlotStore(Block, Level);
    EH.Registration->TryLevelStores.push_back({Address, Address + 7, Level});
  };
  StateBlock(5, 0x1800, 2, 7);
  StateBlock(10, 0x1820, 1, 8);
  StateBlock(11, 0x1840, 1, 8);
  auto &Protected = F.Blocks[7];
  Protected.Id = 7;
  Protected.StartAddr = 0x1810;
  Protected.EndAddr = 0x1811;
  Protected.InstructionBoundaries = {{0x1810, 1}};
  Protected.Succs = {11};
  const auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.CxxContinuationsComplete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  ASSERT_EQ(Result.CxxContinuations.size(), 2u);
  EXPECT_EQ(Result.CxxContinuations[0].TargetVA, 0x1900u);
  EXPECT_EQ(Result.CxxContinuations[1].TargetVA, 0x1820u);
  EXPECT_TRUE(Result.Blocks[10].CallbackOnly);
  EXPECT_EQ(Result.Blocks[10].CxxMinimumTryLevel, 1);
  EXPECT_TRUE(Result.Blocks[9].CallbackOnly);
  EXPECT_EQ(Result.Blocks[9].CxxMinimumTryLevel, 3);
  EXPECT_FALSE(Result.Blocks[6].CallbackOnly);
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
  LowFunc F = makeCxxCatchContinuation();
  F.ExceptionMetadata->Personality = ExceptionPersonality::CxxFrameHandlerX86;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Blocks.size(), 7u);
  EXPECT_EQ(Result.Blocks[5].Levels, (std::vector<int32_t>{1}));
  EXPECT_TRUE(Result.Blocks[5].CallbackOnly);
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

LowFunc makeUnlinkedFrame() {
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
  return F;
}

TEST(RegistrationState, UnlinkEndsTheLiveRegistration) {
  LowFunc F = makeUnlinkedFrame();
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.RegistrationLifetimeComplete);
  ASSERT_TRUE(Result.ChainOperationsComplete);
  ASSERT_EQ(Result.ChainAccesses.size(), 3u);
  EXPECT_EQ(Result.ChainAccesses[0].AccessKind,
            RegistrationChainAccess::Kind::ReadPreviousHead);
  EXPECT_EQ(Result.ChainAccesses[1].AccessKind,
            RegistrationChainAccess::Kind::Install);
  EXPECT_EQ(Result.ChainAccesses[2].AccessKind,
            RegistrationChainAccess::Kind::Remove);
  EXPECT_EQ(Result.ChainAccesses[2].Address, 0x1030u);
  EXPECT_EQ(Result.ChainAccesses[2].EndAddress, 0x1037u);
  EXPECT_EQ(Result.ChainAccesses[2].OpSeq, F.Blocks[3].Ops.back().Seq);
  EXPECT_TRUE(Result.Blocks.back().Levels.empty());
  EXPECT_FALSE(Result.Blocks.back().CanDispatch);
}

TEST(RegistrationState, MissingLifetimeCannotAuthorizeChainReplacement) {
  auto Result = analyzeRegistrationStates(makeBranchingFrame());
  ASSERT_TRUE(Result.Complete);
  EXPECT_FALSE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, ChainOwnershipRequiresUniqueOperationOccurrences) {
  LowFunc F = makeUnlinkedFrame();
  F.Blocks[0].Ops.back().Seq = 3; // Same address/seq as the previous-head load.
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
}

TEST(RegistrationState, ChainOwnershipRequiresUniqueDecodedBoundaries) {
  LowFunc F = makeUnlinkedFrame();
  F.Blocks[0].InstructionBoundaries.push_back({0x1000, 6});
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  ASSERT_TRUE(Result.RegistrationLifetimeComplete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.ChainAccesses.empty());
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

TEST(RegistrationState, AtomicFrameWritesInvalidateEvenWhenTheResultIsDead) {
  for (NdOp Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, Opcode, NdVar::tmp(24, 4),
           {NdVar::tmp(0, 4), NdVar::cst(0, 4), NdVar::cst(1, 4)});
    auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete);
    EXPECT_FALSE(Result.ChainOperationsComplete);
    EXPECT_TRUE(Result.FrameValues.empty());
  }
}

TEST(RegistrationState, AtomicFSWritesCannotBypassChainOwnership) {
  for (NdOp Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, Opcode, NdVar::tmp(24, 4),
           {NdVar::cst(0, 4), NdVar::cst(0, 4), NdVar::cst(1, 4)},
           NdMemoryAddressSpace::X86FS);
    auto Result = analyzeRegistrationStates(F);
    EXPECT_FALSE(Result.Complete);
    EXPECT_FALSE(Result.ChainOperationsComplete);
    EXPECT_TRUE(Result.ChainAccesses.empty());
  }
}

TEST(RegistrationState, PublishesFramePointerProvenanceAfterSpillAndReload) {
  LowFunc F = makeUnlinkedFrame();
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::tmp(0, 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(24, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(24, 4)});
  const int ReloadSeq = F.Blocks[3].Ops.back().Seq;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  auto Reload =
      std::find_if(Result.FrameValues.begin(), Result.FrameValues.end(),
                   [&](const RegistrationFrameValue &Value) {
                     return Value.Address == 0x1030 && Value.OpSeq == ReloadSeq;
                   });
  ASSERT_NE(Reload, Result.FrameValues.end());
  EXPECT_EQ(Reload->EstablishedFrameOffset, -4);
}

TEST(RegistrationState, NarrowSpillsCannotLaunderFramePointerProvenance) {
  LowFunc F = makeUnlinkedFrame();
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(8, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(8, 4), NdVar::reg(x86reg::RBP, 2)});
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_ADD, NdVar::tmp(16, 4),
         {NdVar::tmp(8, 4), NdVar::cst(2, 4)});
  emitOp(F.Blocks[1], 0x1010, NdOp::INT_RIGHT, NdVar::tmp(24, 2),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(16, 1)});
  emitOp(F.Blocks[1], 0x1010, NdOp::STORE, {},
         {NdVar::tmp(16, 4), NdVar::tmp(24, 2)});
  emitOp(F.Blocks[3], 0x1030, NdOp::INT_ADD, NdVar::tmp(24, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-32), 4)});
  emitOp(F.Blocks[3], 0x1030, NdOp::LOAD, NdVar::reg(x86reg::RDI, 4),
         {NdVar::tmp(24, 4)});
  const int ReloadSeq = F.Blocks[3].Ops.back().Seq;
  auto Result = analyzeRegistrationStates(F);
  ASSERT_TRUE(Result.Complete);
  auto Reload =
      std::find_if(Result.FrameValues.begin(), Result.FrameValues.end(),
                   [&](const RegistrationFrameValue &Value) {
                     return Value.Address == 0x1030 && Value.OpSeq == ReloadSeq;
                   });
  ASSERT_NE(Reload, Result.FrameValues.end());
  EXPECT_FALSE(Reload->EstablishedFrameOffset);
}

TEST(RegistrationState, AtomicDesiredValueCannotExportTheRegistrationFrame) {
  LowFunc F = makeUnlinkedFrame();
  emitOp(
      F.Blocks[1], 0x1010, NdOp::ATOMIC_CMPXCHG, NdVar::tmp(24, 4),
      {NdVar::cst(0x800000, 4), NdVar::cst(0, 4), NdVar::reg(x86reg::RBP, 4)});
  auto Result = analyzeRegistrationStates(F);
  EXPECT_FALSE(Result.Complete);
  EXPECT_FALSE(Result.ChainOperationsComplete);
  EXPECT_TRUE(Result.FrameValues.empty());
}

TEST(RegistrationState,
     ImageReadClosureIncludesAtomicsAndRejectsUnknownMemory) {
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    LowFunc F = makeUnlinkedFrame();
    if (Kind == 1)
      emitOp(F.Blocks[1], 0x1010, NdOp::ATOMIC_ADD, NdVar::tmp(24, 4),
             {NdVar::cst(0x800002, 4), NdVar::cst(1, 4)});
    else
      emitOp(
          F.Blocks[1], 0x1010, NdOp::LOAD, NdVar::tmp(24, 4),
          {Kind == 2 ? NdVar::reg(x86reg::RAX, 4) : NdVar::cst(0x800002, 4)});
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    EXPECT_EQ(Result.ImageReadsComplete, Kind != 2);
    if (Kind != 2) {
      ASSERT_EQ(Result.ImageReads.size(), 1u);
      EXPECT_EQ(Result.ImageReads.front().Begin, 0x800002u);
      EXPECT_EQ(Result.ImageReads.front().End, 0x800006u);
    }
  }
}

TEST(RegistrationState, CallsInvalidatePreviouslyClearedVolatileRegisterFacts) {
  for (uint64_t Register : {x86reg::XMM0, x86reg::ZF}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::reg(Register, 1),
           {NdVar::cst(0, 1)});
    emitOp(F.Blocks[1], 0x1010, NdOp::CALL, {}, {NdVar::cst(0x900000, 4)});
    emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::tmp(24, 1),
           {NdVar::reg(Register, 1)});
    const int Seq = F.Blocks[1].Ops.back().Seq;
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    auto Value = llvm::find_if(Result.FrameValues, [&](const auto &V) {
      return V.Address == 0x1010 && V.OpSeq == Seq;
    });
    ASSERT_NE(Value, Result.FrameValues.end());
    EXPECT_FALSE(Value->EstablishedFrameOffset);
  }
}

TEST(RegistrationState, OpaqueEntryRegistersKeepTheirPossibleFrameIdentity) {
  for (uint64_t Register :
       {x86reg::RAX, x86reg::RCX, x86reg::RBX, x86reg::ZF}) {
    LowFunc F = makeUnlinkedFrame();
    emitOp(F.Blocks[1], 0x1010, NdOp::COPY, NdVar::tmp(24, 1),
           {NdVar::reg(Register, 1)});
    const int Seq = F.Blocks[1].Ops.back().Seq;
    auto Result = analyzeRegistrationStates(F);
    ASSERT_TRUE(Result.Complete);
    auto Value = llvm::find_if(Result.FrameValues, [&](const auto &V) {
      return V.Address == 0x1010 && V.OpSeq == Seq;
    });
    ASSERT_NE(Value, Result.FrameValues.end());
    EXPECT_FALSE(Value->EstablishedFrameOffset);
  }
}

} // namespace

//===- LowIRConcolicMemoryTests.cpp - Concrete entry RAM ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LowIRConcolicDetail.h"
#include "gtest/gtest.h"

#include "neverd/concolic/LowIRConcolic.h"

using namespace neverd;
using namespace neverd::concolic;
using namespace neverd::symbolic;

namespace {

constexpr va_t Code = 0x400000;
constexpr uint64_t Memory = 0x600000;

LowOp operation(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
                int Seq) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Code;
  Op.Seq = Seq;
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  return Op;
}

LowFunc memoryBranch(uint64_t Address = Memory, bool AddRegister = false) {
  LowFunc Function;
  Function.Entry = Code;
  Function.Name = "entry_memory_branch";
  LowBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = Code;
  Entry.EndAddr = Code + 4;
  Entry.Succs = {1, 2};
  Entry.Ops.push_back(
      operation(NdOp::LOAD, NdVar::reg(16, 2), {NdVar::cst(Address, 8)}, 0));
  if (AddRegister)
    Entry.Ops.push_back(operation(NdOp::INT_ADD, NdVar::reg(16, 2),
                                  {NdVar::reg(16, 2), NdVar::reg(0, 2)}, 1));
  Entry.Ops.push_back(operation(NdOp::INT_EQUAL, NdVar::reg(64, 1),
                                {NdVar::reg(16, 2), NdVar::cst(0x1234, 2)}, 2));
  Entry.Ops.push_back(operation(
      NdOp::COND_BR, NdVar{}, {NdVar::cst(Code + 4, 8), NdVar::reg(64, 1)}, 3));
  Function.Blocks.push_back(std::move(Entry));
  for (int Id : {1, 2}) {
    LowBlock Exit;
    Exit.Id = Id;
    Exit.StartAddr = Code + uint64_t(Id) * 4;
    Exit.EndAddr = Exit.StartAddr + 4;
    Exit.Ops.push_back(operation(NdOp::RETURN, NdVar{}, {}, 0));
    Exit.Ops.back().Addr = Exit.StartAddr;
    Function.Blocks.push_back(std::move(Exit));
  }
  return Function;
}

TEST(LowIRConcolicMemory, ProjectsInputBytesAndFreshlyReplaysBothByteOrders) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    LowIRConcolicOptions Options;
    Options.ByteOrder = Order;
    Options.InitialSeed = {{0, 8, 0x8877665544332211}};
    Options.InitialMemory = {{Memory + 1, {0, 0xAB}}, {Memory, {0}}};
    const auto Report = runLowIRConcolic(memoryBranch(), Options);
    ASSERT_TRUE(Report.TraceExact);
    EXPECT_TRUE(Report.TraceComplete);
    EXPECT_FALSE(Report.Exhaustive);
    EXPECT_EQ(Report.Version, 2u);
    EXPECT_EQ(Report.Blocks, (std::vector<int>{0, 2}));
    ASSERT_EQ(Report.Flips.size(), 1u);
    EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::Verified);
    EXPECT_EQ(Report.Flips[0].ReplayReason, ConcolicReplayReason::None);
    ASSERT_EQ(Report.Candidates.size(), 1u);
    const auto &Candidate = Report.Candidates.front();
    ASSERT_EQ(Candidate.Seed.size(), 1u);
    EXPECT_EQ(Candidate.Seed[0].Value, Options.InitialSeed[0].Value);
    ASSERT_EQ(Candidate.MemorySeed.size(), 2u);
    EXPECT_EQ(Candidate.MemorySeed[0].Address, Memory);
    EXPECT_EQ(Candidate.MemorySeed[1].Address, Memory + 1);
    EXPECT_EQ(Candidate.MemorySeed[0].Bytes,
              (std::vector<uint8_t>{Order == llvm::endianness::little
                                        ? uint8_t(0x34)
                                        : uint8_t(0x12)}));
    EXPECT_EQ(
        Candidate.MemorySeed[1].Bytes,
        (std::vector<uint8_t>{Order == llvm::endianness::little ? uint8_t(0x12)
                                                                : uint8_t(0x34),
                              0xAB}));
    EXPECT_EQ(Options.InitialMemory[0].Bytes, (std::vector<uint8_t>{0, 0xAB}));

    Options.InitialSeed = Candidate.Seed;
    Options.InitialMemory = Candidate.MemorySeed;
    Options.MaxFlipAttempts = 0;
    const auto Replay = runLowIRConcolic(memoryBranch(), Options);
    ASSERT_TRUE(Replay.TraceExact);
    EXPECT_EQ(Replay.Blocks, (std::vector<int>{0, 1}));
  }
}

TEST(LowIRConcolicMemory, RegisterAndMemoryInputsCanShareNumericOffsets) {
  LowIRConcolicOptions Options;
  Options.InitialSeed = {{0, 2, 1}};
  Options.InitialMemory = {{0, {2, 0}}};
  const auto Report = runLowIRConcolic(memoryBranch(0, true), Options);
  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Candidates.size(), 1u);
  const auto &Candidate = Report.Candidates.front();
  ASSERT_EQ(Candidate.Seed.size(), 1u);
  ASSERT_EQ(Candidate.MemorySeed.size(), 1u);
  const auto &Bytes = Candidate.MemorySeed[0].Bytes;
  ASSERT_EQ(Bytes.size(), 2u);
  const uint16_t Word = uint16_t(Bytes[0]) | (uint16_t(Bytes[1]) << 8);
  EXPECT_EQ(uint16_t(Word + Candidate.Seed[0].Value), 0x1234u);
}

TEST(LowIRConcolicMemory, StoresOverrideSnapshotsWithoutChangingEntryOrigins) {
  LowFunc Function = memoryBranch();
  auto &Ops = Function.Blocks.front().Ops;
  Ops.insert(Ops.begin() + 1,
             operation(NdOp::STORE, NdVar{},
                       {NdVar::cst(Memory, 8), NdVar::cst(7, 2)}, 4));
  Ops.insert(Ops.begin() + 2, operation(NdOp::LOAD, NdVar::reg(24, 2),
                                        {NdVar::cst(Memory, 8)}, 5));
  Ops.insert(Ops.begin() + 3,
             operation(NdOp::INT_ADD, NdVar::reg(16, 2),
                       {NdVar::reg(16, 2), NdVar::reg(24, 2)}, 6));
  LowIRConcolicOptions Options;
  Options.InitialMemory = {{Memory, {0, 0, 0xAB}}};
  const auto Report = runLowIRConcolic(Function, Options);
  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Candidates.size(), 1u);
  EXPECT_EQ(Report.Candidates[0].MemorySeed[0].Bytes,
            (std::vector<uint8_t>{0x2D, 0x12, 0xAB}));
}

TEST(LowIRConcolicMemory, MissingBytesAndSymbolicAddressesKeepTheirRefusals) {
  LowIRConcolicOptions Options;
  Options.InitialMemory = {{Memory, {0}}};
  auto Report = runLowIRConcolic(memoryBranch(), Options);
  EXPECT_FALSE(Report.TraceExact);
  EXPECT_TRUE(Report.Candidates.empty());
  EXPECT_EQ(Report.TraceReason, ConcolicTraceReason::IncompleteConcreteTrace);

  LowFunc Function = memoryBranch();
  Function.Blocks[0].Ops[0].Inputs[0] = NdVar::reg(0, 8);
  Options.InitialSeed = {{0, 8, Memory}};
  Options.InitialMemory = {{Memory, {0, 0}}};
  Report = runLowIRConcolic(Function, Options);
  ASSERT_TRUE(Report.TraceExact);
  ASSERT_EQ(Report.Flips.size(), 1u);
  EXPECT_EQ(Report.Flips[0].Status, ConcolicFlipStatus::ProjectionRejected);
  EXPECT_EQ(Report.Flips[0].ProjectionReason,
            ConcolicProjectionReason::MissingInputOrigin);
  EXPECT_TRUE(Report.Candidates.empty());
}

TEST(LowIRConcolicMemory, MalformedAndOverBudgetSnapshotsNeverExecute) {
  for (const std::vector<SymConcreteMemory> Seed : {
           std::vector<SymConcreteMemory>{{Memory, {}}},
           {{UINT64_MAX, {1, 2}}},
           {{Memory, {1, 2}}, {Memory + 1, {2}}},
           {{Memory + 1, {2}}, {Memory, {1, 2}}},
           {{Memory, {1}}, {Memory, {1}}},
       }) {
    LowIRConcolicOptions Options;
    Options.InitialMemory = Seed;
    const auto Report = runLowIRConcolic(memoryBranch(), Options);
    EXPECT_EQ(Report.TraceReason, ConcolicTraceReason::InvalidInitialSeed);
    EXPECT_EQ(Report.ExecutedSteps, 0u);
    EXPECT_TRUE(Report.Candidates.empty());
  }
  for (size_t Limit : {0u, 1u, 3u, 4u}) {
    LowIRConcolicOptions Options;
    Options.InitialMemory = {{Memory, {0, 0}}, {Memory + 2, {0xAB, 0xCD}}};
    Options.MaxMemorySeedBytes = Limit;
    const auto Report = runLowIRConcolic(memoryBranch(), Options);
    if (Limit == 4) {
      EXPECT_TRUE(Report.TraceExact);
      EXPECT_EQ(Report.Candidates.size(), 1u);
    } else {
      EXPECT_EQ(Report.TraceReason,
                ConcolicTraceReason::MemorySeedBudgetExceeded);
      EXPECT_EQ(Report.ExecutedSteps, 0u);
      EXPECT_TRUE(Report.InitialMemory.empty());
      EXPECT_TRUE(Report.Candidates.empty());
    }
  }
}

TEST(LowIRConcolicMemory, ProjectionRequiresEpochZeroCoveredExactModelBytes) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    SymContext Ctx;
    SymInputOrigin Origin{SymInputKind::AbsoluteMemory, Memory, 1, 0};
    if (Case == 0)
      Origin.Epoch = 1;
    if (Case == 1)
      ++Origin.Offset;
    SymRef Input = Ctx.mkInputVar("memory", 8, Origin);
    SymRef Query = Ctx.mkEq(Input, Ctx.mkConst(8, 3));
    solver::BitVectorModel Model;
    Model.set(Ctx.varId(Input),
              llvm::APInt(Case == 2 ? 16 : 8, Case == 3 ? 2 : 3));
    const auto Projected = detail::projectInputModel(
        Ctx, Query, Model, {}, llvm::endianness::little, {{Memory, {0}}});
    const ConcolicProjectionReason Expected[] = {
        ConcolicProjectionReason::NonzeroInputEpoch,
        ConcolicProjectionReason::MissingBaselineByte,
        ConcolicProjectionReason::InvalidInputWidth,
        ConcolicProjectionReason::CandidateDoesNotSatisfyQuery};
    EXPECT_EQ(Projected.Reason, Expected[Case]);
    EXPECT_TRUE(Projected.Seed.empty());
    EXPECT_TRUE(Projected.MemorySeed.empty());
  }
}

TEST(LowIRConcolicMemory, CandidateIdentityIncludesEveryMemoryByte) {
  std::vector<LowIRConcolicCandidate> Published;
  EXPECT_EQ(detail::publishReplayVerifiedSeed(Published, {}, 1, {{Memory, {1}}})
                .Status,
            ConcolicFlipStatus::Verified);
  EXPECT_EQ(detail::publishReplayVerifiedSeed(Published, {}, 1, {{Memory, {1}}})
                .Status,
            ConcolicFlipStatus::VerifiedDuplicate);
  EXPECT_EQ(detail::publishReplayVerifiedSeed(Published, {}, 1, {{Memory, {2}}})
                .Status,
            ConcolicFlipStatus::CandidateBudgetExceeded);
  EXPECT_EQ(
      detail::publishReplayVerifiedSeed(Published, {}, 2, {{Memory + 1, {1}}})
          .Status,
      ConcolicFlipStatus::Verified);
  ASSERT_EQ(Published.size(), 2u);
}

} // namespace

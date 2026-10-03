#include "gtest/gtest.h"

#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/lift/AArch64Regs.h"

#include <algorithm>

using namespace neverd;
namespace {
const auto SP = NdVar::reg(a64reg::SP, 8);
const auto Slot = NdVar::reg(a64reg::X19, 8);
const auto Value = NdVar::reg(a64reg::X21, 8);
const auto X0 = NdVar::reg(a64reg::X0, 8);

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
         va_t Address) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.Addr = Address;
  for (const auto &Input : Inputs)
    Result.addInput(Input);
  return Result;
}
struct FrameFixture {
  LowFunc Low;
  FrameFixture() {
    Low.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 7;
    Block.StartAddr = Low.Entry;
    Block.Ops = {
        op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1000),
        op(NdOp::COPY, Value, {NdVar::addressFragment(0x6000, 8)}, 0x1004),
        op(NdOp::INT_ADD, Value, {Value, NdVar::scalar(128, 8)}, 0x1008),
        op(NdOp::INT_ADD, Slot, {SP, NdVar::scalar(32, 8)}, 0x100c),
        op(NdOp::STORE, {}, {Slot, Value}, 0x1010),
        op(NdOp::COPY, Value, {NdVar::scalar(0, 8)}, 0x1014),
        op(NdOp::LOAD, NdVar::tmp(16, 8), {Slot}, 0x1018),
        // Later unknown cleanup does not authorize this earlier query.
        op(NdOp::CALL, X0, {NdVar::cst(0x5000, 8)}, 0x101c),
        op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1020)};
    Low.Blocks.push_back(std::move(Block));
  }
  std::optional<SourceFrameLoadDefinition>
  query(const NativeSourceCalls &Calls = {}) const {
    for (const auto &Block : Low.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        if (Block.Ops[I].Addr == 0x1018 && Block.Ops[I].Opcode == NdOp::LOAD)
          return sourceFrameLoadedDefinition(Low, Arch::AArch64, Calls,
                                             Block.Id, I);
    return std::nullopt;
  }
  void prefix(std::initializer_list<LowOp> Ops) {
    auto &B = Low.Blocks.front();
    B.Ops.insert(B.Ops.begin() + 6, Ops);
  }
  void diamond() {
    const auto Original = Low.Blocks.front();
    Low.Blocks.resize(4);
    for (size_t I = 0; I < 4; ++I) {
      Low.Blocks[I] = {};
      Low.Blocks[I].Id = 7 + I;
      Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    }
    Low.Blocks[0].Ops.assign(Original.Ops.begin(), Original.Ops.begin() + 6);
    Low.Blocks[0].Succs = {8, 9};
    for (size_t I : {1, 2}) {
      Low.Blocks[I].Preds = {7};
      Low.Blocks[I].Succs = {10};
      Low.Blocks[I].Ops = {op(NdOp::COPY, NdVar::reg(a64reg::X9, 8),
                              {NdVar::scalar(0, 8)}, 0x1000 + I * 0x100)};
    }
    Low.Blocks[3].Preds = {8, 9};
    Low.Blocks[3].Ops.assign(Original.Ops.begin() + 6, Original.Ops.end());
  }
};

SourceFunctionTypeHint signature(size_t Parameters = 1,
                                 bool ReturnsPointer = false) {
  SourceFunctionTypeHint Result;
  Result.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Result.ReturnType =
      ReturnsPointer ? NdType::makePtr(NdType::makeVoid()) : NdType::makeVoid();
  for (size_t I = 0; I < Parameters; ++I)
    Result.Parameters.push_back({"arg", NdType::makeInt(8, false)});
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Result, Arch::AArch64, Error))
      << Error;
  return Result;
}
NativeSourceCalls callsAt(va_t Address, const SourceFunctionTypeHint &Signature,
                          const SourceFrameEffects &Effects) {
  NativeSourceCallContract Contract;
  Contract.Signature = &Signature;
  static_cast<SourceFrameEffects &>(Contract) = Effects;
  const auto Call = op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, Address);
  return {{*nativeSourceCallKey(Call), Contract}};
}
} // namespace

TEST(SourceFrameAnalysis,
     ReturnsTheOriginalDefinitionWithoutPublishingAnAddress) {
  FrameFixture F;
  const auto Result = F.query();
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->FrameOffset, -32);
  EXPECT_EQ(Result->Definition,
            (SourceFrameDefinition{7, 2, 0x1008, 0, Value}));
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, {}));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::X64, {}, 7, 6));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 8, 6));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 7, 99));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 7, 5));
  // Numerically equal scalar constants do not inherit an instruction's origin.
  F.Low.Blocks[0].Ops[2] =
      op(NdOp::COPY, Value, {NdVar::scalar(0x6080, 8)}, 0x1008);
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, RejectsPartialEscapedExpiredAndUninitializedBytes) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    switch (Case) {
    case 0:
      Ops.erase(Ops.begin() + 4);
      break;
    case 1:
      Ops[4].Inputs[1].Size = 4;
      break;
    case 2:
      Ops[6].Output.Size = 4;
      break;
    case 3:
      Ops[4].Inputs[1] = SP;
      break;
    case 4:
      F.prefix({op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1015)});
      break;
    case 5:
      F.prefix({op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(64, 8)}, 0x1015),
                op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1016)});
      break;
    case 6:
      F.prefix({op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1015)});
      break;
    case 7:
      F.prefix({op(NdOp::COPY, Slot, {NdVar::reg(a64reg::X20, 8)}, 0x1015)});
      break;
    case 8:
      Ops[6].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      F.prefix({op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), SP}, 0x1015)});
      break;
    case 10:
      F.prefix(
          {op(NdOp::COPY, NdVar::reg(a64reg::X9, 4),
              {NdVar::reg(a64reg::SP, 4)}, 0x1015),
           op(NdOp::STORE, {}, {Slot, NdVar::reg(a64reg::X9, 8)}, 0x1016)});
      break;
    case 11:
      Ops[3].Inputs[1] = NdVar::scalar(64, 8);
      break;
    case 12:
      Ops[3].Inputs[1] = NdVar::scalar(uint64_t(-8), 8);
      break;
    case 13:
      F.prefix({op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1015)});
      break;
    case 14:
      Ops[4].Inputs[0].Size = 4;
      break;
    case 15:
      Ops[6].NumInputs = 0;
      break;
    case 16:
      F.prefix({op(NdOp::INT_XOR, Slot, {Slot, NdVar::scalar(1, 8)}, 0x1015)});
      break;
    case 17:
      F.prefix({op(NdOp::COPY, NdVar::reg(a64reg::X19, 4),
                   {NdVar::scalar(0, 4)}, 0x1015)});
      break;
    }
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, RequiresAllReachingPathsIndependentOfBlockOrder) {
  FrameFixture F;
  F.diamond();
  const auto Expected = F.query();
  ASSERT_TRUE(Expected);
  std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
  EXPECT_EQ(F.query(), Expected);
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture Bad;
    Bad.diamond();
    auto &B = Bad.Low.Blocks;
    switch (Case) {
    case 0:
      B[2].Ops.push_back(
          op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 8)}, 0x1204));
      break;
    case 1:
      B[2].Ops.push_back(
          op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), SP}, 0x1204));
      break;
    case 2:
      B[0].Ops.erase(B[0].Ops.begin() + 4);
      B[1].Ops.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1104));
      break;
    case 3:
      B[3].Preds.pop_back();
      break;
    case 4:
      B[3].Preds.push_back(8);
      break;
    case 5:
      B[2].Succs.push_back(9);
      B[2].Preds.push_back(9);
      break;
    case 6:
      B[3].Succs = {7};
      B[0].Preds = {10};
      break;
    case 7:
      B[1].Id = 9;
      break;
    case 8:
      B[2].Ops.clear();
      break;
    case 9:
      B[0].Succs = {8};
      B[2].Preds.clear();
      break;
    }
    // A loop which writes only an unrelated register preserves the slot.
    EXPECT_EQ(bool(Bad.query()), Case == 5);
  }
  // A loop reached only after the query is irrelevant to this prefix.
  F = FrameFixture{};
  auto &Root = F.Low.Blocks[0];
  Root.Ops.resize(7);
  Root.Succs = {8};
  LowBlock Later;
  Later.Id = 8;
  Later.StartAddr = 0x2000;
  Later.Preds = {7, 8};
  Later.Succs = {8};
  Later.Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x2000, 8)}, 0x2000)};
  F.Low.Blocks.push_back(Later);
  EXPECT_TRUE(F.query());
}

TEST(SourceFrameAnalysis, UsesBoundedCallWritesAndPossibleReturnAliases) {
  auto Signature = signature(1, true);
  for (unsigned Case = 0; Case < 11; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    SourceFrameEffects Effects;
    Effects.WritableFrameParameters[0] = 24;
    F.prefix({op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(0, 8)}, 0x1015),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1016)});
    switch (Case) {
    case 1:
      Effects.WritableFrameParameters[0] = 33;
      break;
    case 2:
      Effects = {};
      break;
    case 3:
      Effects.WritableFrameParameters[0] = 65;
      break;
    case 4:
      Effects.ReadOnlyFrameParameters[0] = 24;
      break;
    case 5:
      Effects.ReturnFrameOrExternal = {0, 24};
      break;
    case 6:
      Effects.ReturnFrameOrExternal = {0, 24};
      F.Low.Blocks[0].Ops[8].Inputs[0] = X0;
      break;
    case 7:
      Effects.ReturnFrameOrExternal = {0, 24};
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 8,
                                 op(NdOp::STORE, {}, {Slot, X0}, 0x1017));
      break;
    case 8:
      Effects.WritableFrameParameters[0] = 0;
      break;
    case 9:
      Effects = {};
      Effects.ReadOnlyFrameParameters[0] = 40;
      break;
    case 10:
      F.Low.Blocks[0].Ops[6].Inputs[1] = NdVar::scalar(16, 8);
      break;
    }
    EXPECT_EQ(bool(F.query(callsAt(0x1016, Signature, Effects))),
              Case == 0 || Case == 5 || Case == 9);
  }
}

TEST(SourceFrameAnalysis, RetainsConditionalScratchAndLiveRecordRequirements) {
  auto Begin = signature(4);
  auto End = signature(1);
  SourceFrameEffects Initialize, Finish;
  using Scratch = SourceFrameScratchEffect;
  Initialize.WritableFrameParameters[1] = 24;
  Initialize.Scratch = {Scratch::Domain::SwiftAccess,
                        Scratch::Action::Initialize, 1, 24,
                        SourceFrameScalarCondition{2, {0, 1}}};
  Finish.ReadOnlyFrameParameters[0] = 24;
  Finish.Scratch = {Scratch::Domain::SwiftAccess, Scratch::Action::Finish, 0,
                    24, std::nullopt};
  auto Calls = callsAt(0x1034, Begin, Initialize);
  Calls.merge(callsAt(0x103c, End, Finish));
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    const auto X1 = NdVar::reg(a64reg::X1, 8);
    const auto X2 = NdVar::reg(a64reg::X2, 8);
    F.prefix({op(NdOp::COPY, X0, {NdVar::scalar(0, 8)}, 0x1024),
              op(NdOp::COPY, X1, {SP}, 0x1028),
              op(NdOp::COPY, X2,
                 {NdVar::scalar(Case == 1   ? 1
                                : Case == 2 ? 32
                                            : 0,
                                8)},
                 0x102c),
              op(NdOp::COPY, NdVar::reg(a64reg::X3, 8), {NdVar::scalar(0, 8)},
                 0x1030),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1034),
              op(NdOp::COPY, X0, {SP}, 0x1038),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x103c)});
    auto &Ops = F.Low.Blocks[0].Ops;
    auto Contract = Calls;
    switch (Case) {
    case 3:
      Ops[8].Inputs[0] = NdVar::reg(a64reg::X9, 8);
      break;
    case 4:
      Ops[8].Inputs[0].Size = 4;
      break;
    case 5:
      Ops.erase(Ops.begin() + 10);
      break;
    case 6:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::STORE, {}, {SP, NdVar::scalar(0, 1)}, 0x1039));
      break;
    case 7:
      Ops[11] = op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(8, 8)}, 0x1038);
      break;
    case 8:
      Ops[7] = op(NdOp::INT_ADD, X1, {SP, NdVar::scalar(16, 8)}, 0x1028);
      break;
    case 9:
      Contract.begin()->second.Scratch->Condition->Parameter = 4;
      break;
    case 10:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1039));
      break;
    case 11:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)}, 0x1039));
      break;
    }
    EXPECT_EQ(bool(F.query(Contract)), Case == 0 || Case == 1);
  }
}

namespace {
FrameFixture loopFrame(bool QueryInLoop) {
  FrameFixture F;
  const auto Original = F.Low.Blocks[0];
  F.Low.Blocks.resize(4);
  for (size_t I = 0; I < 4; ++I) {
    F.Low.Blocks[I] = {};
    F.Low.Blocks[I].Id = 7 + I;
    F.Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
  }
  auto &Entry = F.Low.Blocks[0];
  Entry.Ops.assign(Original.Ops.begin(), Original.Ops.begin() + 6);
  Entry.Succs = {8};
  auto &Header = F.Low.Blocks[1];
  Header.Preds = {7, 9};
  Header.Succs = {9, 10};
  Header.Ops = {
      op(NdOp::COPY, NdVar::reg(a64reg::X9, 8), {NdVar::scalar(0, 8)}, 0x1100)};
  auto &Body = F.Low.Blocks[2];
  Body.Preds = {8};
  Body.Succs = {8};
  Body.Ops = {op(NdOp::COPY, NdVar::reg(a64reg::X10, 8), {NdVar::scalar(1, 8)},
                 0x1200)};
  auto &Exit = F.Low.Blocks[3];
  Exit.Preds = {8};
  Exit.Ops.assign(Original.Ops.begin() + 7, Original.Ops.end());
  auto &QueryOps = QueryInLoop ? Header.Ops : Exit.Ops;
  QueryOps.insert(QueryOps.begin(), Original.Ops[6]);
  return F;
}
} // namespace

TEST(SourceFrameAnalysis, PreservesOnePreloopDefinitionAcrossAllIterations) {
  for (bool Inside : {false, true}) {
    SCOPED_TRACE(Inside);
    auto F = loopFrame(Inside);
    const auto Expected = F.query();
    ASSERT_TRUE(Expected);
    EXPECT_EQ(Expected->Definition,
              (SourceFrameDefinition{7, 2, 0x1008, 0, Value}));
    EXPECT_EQ(Expected->FrameOffset, -32);
    std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
    EXPECT_EQ(F.query(), Expected);
  }
}

TEST(SourceFrameAnalysis, LoopQueryRequiresTheCompleteBackedgeEffects) {
  const auto Signature = signature();
  SourceFrameEffects ReadOnly;
  ReadOnly.ReadOnlyFrameParameters[0] = 8;
  SourceFrameEffects Written;
  Written.WritableFrameParameters[0] = 8;
  for (bool Inside : {false, true}) {
    SCOPED_TRACE(Inside);
    auto F = loopFrame(Inside);
    F.Low.Blocks[2].Ops = {op(NdOp::COPY, X0, {Slot}, 0x1200),
                           op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1204)};
    EXPECT_TRUE(F.query(callsAt(0x1204, Signature, ReadOnly)));
    EXPECT_FALSE(F.query());
    EXPECT_FALSE(F.query(callsAt(0x1204, Signature, {})));
    EXPECT_FALSE(F.query(callsAt(0x1204, Signature, Written)));
    // Calls after the queried LOAD still affect the next iteration.
    if (Inside) {
      F.Low.Blocks[1].Ops.insert(F.Low.Blocks[1].Ops.end(),
                                 F.Low.Blocks[2].Ops.begin(),
                                 F.Low.Blocks[2].Ops.end());
      F.Low.Blocks[2].Ops = {op(NdOp::COPY, X0, {NdVar::scalar(0, 8)}, 0x1300)};
      EXPECT_TRUE(F.query(callsAt(0x1204, Signature, ReadOnly)));
      EXPECT_FALSE(F.query());
      EXPECT_FALSE(F.query(callsAt(0x1204, Signature, Written)));
    }
  }
}

TEST(SourceFrameAnalysis,
     LoopReloadRejectsChangedExpiredEscapedOrPartialBytes) {
  for (bool Inside : {false, true})
    for (unsigned Case = 0; Case < 13; ++Case) {
      SCOPED_TRACE(Inside);
      SCOPED_TRACE(Case);
      auto F = loopFrame(Inside);
      auto &Body = F.Low.Blocks[2].Ops;
      switch (Case) {
      case 0:
        Body.push_back(
            op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 8)}, 0x1204));
        break;
      case 1:
        Body.push_back(
            op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1204));
        break;
      case 2:
        Body.push_back(op(NdOp::STORE, {}, {Slot, SP}, 0x1204));
        break;
      case 3:
        Body.push_back(
            op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), Slot}, 0x1204));
        break;
      case 4:
        Body.push_back(
            op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(64, 8)}, 0x1204));
        Body.push_back(
            op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1208));
        break;
      case 5:
        Body.push_back(
            op(NdOp::COPY, Slot, {NdVar::reg(a64reg::X20, 8)}, 0x1204));
        break;
      case 6:
        Body.push_back(op(NdOp::COPY, NdVar::reg(a64reg::X19, 4),
                          {NdVar::scalar(0, 4)}, 0x1204));
        break;
      case 7:
        Body.push_back(op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x1204));
        break;
      case 8:
        F.Low.Blocks[0].Ops.erase(F.Low.Blocks[0].Ops.begin() + 4);
        Body.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1204));
        break;
      case 9:
        Body.push_back(
            op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1204));
        Body.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1208));
        break;
      case 10:
        Body.push_back(
            op(NdOp::INT_SUB, SP, {SP, NdVar::reg(a64reg::X20, 8)}, 0x1204));
        break;
      case 11:
        F.Low.Blocks[1].Preds.pop_back();
        break;
      case 12:
        Body.push_back(op(
            NdOp::STORE, {},
            {NdVar::reg(a64reg::X20, 8), NdVar::reg(a64reg::X19, 4)}, 0x1204));
        break;
      }
      EXPECT_FALSE(F.query());
      std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_FALSE(F.query());
    }
}

TEST(SourceFrameAnalysis, DoesNotEquateDifferentExecutionsOfALoopProducer) {
  for (unsigned Case = 0; Case < 3; ++Case) {
    SCOPED_TRACE(Case);
    auto F = loopFrame(true);
    auto &Header = F.Low.Blocks[1].Ops;
    std::vector<LowOp> Producer = {
        op(NdOp::LOAD, Value, {NdVar::reg(a64reg::X20, 8)}, 0x1100),
        op(NdOp::STORE, {}, {Slot, Value}, 0x1104)};
    if (Case == 1)
      Producer[0] =
          op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1100);
    Header.insert(Header.begin(), Producer.begin(), Producer.end());
    if (Case == 2) {
      // The same static load may also execute in an earlier iteration before
      // a later query block. A definition site alone proves no value identity.
      Header.erase(Header.begin() + 2);
      F.Low.Blocks[3].Ops.insert(
          F.Low.Blocks[3].Ops.begin(),
          op(NdOp::LOAD, NdVar::tmp(16, 8), {Slot}, 0x1018));
    }
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, LoopQueryRetainsScratchConditionsAndObligations) {
  using Scratch = SourceFrameScratchEffect;
  const auto Begin = signature(4), End = signature();
  SourceFrameEffects Initialize, Finish;
  Initialize.WritableFrameParameters[1] = 24;
  Initialize.Scratch = {Scratch::Domain::SwiftAccess,
                        Scratch::Action::Initialize,
                        1,
                        24,
                        SourceFrameScalarCondition{2, {0, 1, 32, 33}},
                        {32, 33}};
  Finish.ReadOnlyFrameParameters[0] = 24;
  Finish.Scratch = {Scratch::Domain::SwiftAccess,
                    Scratch::Action::Finish,
                    0,
                    24,
                    std::nullopt,
                    {}};
  auto Calls = callsAt(0x1210, Begin, Initialize);
  Calls.merge(callsAt(0x1218, End, Finish));
  for (bool Inside : {false, true})
    for (unsigned Case = 0; Case < 6; ++Case) {
      SCOPED_TRACE(Inside);
      SCOPED_TRACE(Case);
      auto F = loopFrame(Inside);
      const uint64_t Flags = Case == 2 ? 0 : Case == 3 ? 4 : 32;
      auto &Body = F.Low.Blocks[2].Ops;
      Body = {op(NdOp::COPY, X0, {NdVar::reg(a64reg::X20, 8)}, 0x1200),
              op(NdOp::COPY, NdVar::reg(a64reg::X1, 8), {SP}, 0x1204),
              op(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                 {NdVar::scalar(Flags, 8)}, 0x1208),
              op(NdOp::COPY, NdVar::reg(a64reg::X3, 8), {NdVar::scalar(0, 8)},
                 0x120c),
              op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1210),
              op(NdOp::COPY, X0, {SP}, 0x1214),
              op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1218)};
      if (Case == 1 || Case == 2)
        Body.resize(5);
      if (Case == 4)
        Body.insert(Body.begin() + 5,
                    op(NdOp::STORE, {}, {SP, NdVar::scalar(0, 1)}, 0x1212));
      if (Case == 5)
        Body[5] = op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(8, 8)}, 0x1214);
      EXPECT_EQ(bool(F.query(Calls)), Case == 0 || Case == 2);
    }
}

TEST(SourceFrameAnalysis, BoundsDefinitionAndTransferWork) {
  FrameFixture F;
  std::vector<LowOp> Prefix;
  for (size_t I = 0; I < 4097; ++I)
    Prefix.push_back(op(NdOp::COPY, Value,
                        {NdVar::addressFragment(0x6000 + I, 8)},
                        0x4000 + I * 4));
  F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 6, Prefix.begin(),
                             Prefix.end());
  EXPECT_FALSE(F.query());
  F = FrameFixture{};
  F.Low.Blocks.resize(16385);
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, JoinsOnlyOrderedBytesOfOneDefinition) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    const auto LowHalf = NdVar::reg(a64reg::X10, 4);
    const auto HighHalf = NdVar::reg(a64reg::X11, 4);
    const auto NextSlot = NdVar::reg(a64reg::X12, 8);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.erase(Ops.begin() + 4);
    Ops.insert(Ops.begin() + 4,
               {op(NdOp::SUBBYTES, LowHalf,
                   {Value, NdVar::scalar(Case == 1 ? 4 : 0, 4)}, 0x1010),
                op(NdOp::SUBBYTES, HighHalf,
                   {Value, NdVar::scalar(Case == 1 ? 0 : 4, 4)}, 0x1011),
                op(NdOp::INT_ADD, NextSlot,
                   {Slot, NdVar::scalar(Case == 2 ? 3 : 4, 8)}, 0x1012),
                op(NdOp::STORE, {}, {Slot, LowHalf}, 0x1013),
                op(NdOp::STORE, {}, {NextSlot, HighHalf}, 0x1014)});
    if (Case == 3)
      Ops.insert(
          Ops.begin() + 5,
          op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1010));
    const auto Result = F.query();
    EXPECT_EQ(bool(Result), Case == 0);
    if (Result)
      EXPECT_EQ(Result->Definition.Instruction, 0x1008U);
  }
}

namespace {
// These LowIR tests isolate the shared lifetime transfer. Machine/import/ABI
// authentication is exercised independently by NativeSourceHints' real fixture.
struct AccessLifetimeFixture {
  LowFunc Low;
  NativeSourceCalls Calls;
  SourceFunctionTypeHint Begin = signature(4), End = signature(1),
                         Ordinary = signature(0), Borrow = signature(1);
  SourceFrameEffects Initialize, Finish;
  const NdVar X1 = NdVar::reg(a64reg::X1, 8), X2 = NdVar::reg(a64reg::X2, 8),
              X3 = NdVar::reg(a64reg::X3, 8), X9 = NdVar::reg(a64reg::X9, 8),
              X20 = NdVar::reg(a64reg::X20, 8), LR = NdVar::reg(a64reg::X30, 8);
  va_t Next = 0x1000;
  AccessLifetimeFixture() {
    using Scratch = SourceFrameScratchEffect;
    Initialize.WritableFrameParameters[1] = 24;
    Initialize.Scratch = {Scratch::Domain::SwiftAccess,
                          Scratch::Action::Initialize,
                          1,
                          24,
                          SourceFrameScalarCondition{2, {0, 1, 32, 33}},
                          {32, 33}};
    Finish.ReadOnlyFrameParameters[0] = 24;
    Finish.Scratch = {Scratch::Domain::SwiftAccess,
                      Scratch::Action::Finish,
                      0,
                      24,
                      std::nullopt,
                      {}};
    Low.Entry = Next;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    Low.Blocks[0].StartAddr = Next;
    add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(96, 8)});
    address(X9, 80);
    add(NdOp::STORE, {}, {X9, LR});
  }
  void add(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
    Low.Blocks[0].Ops.push_back(op(Opcode, Output, Inputs, Next));
    Next += 4;
  }
  void address(NdVar Output, uint64_t Offset) {
    add(NdOp::INT_ADD, Output, {SP, NdVar::scalar(Offset, 8)});
  }
  void call(const SourceFunctionTypeHint &Type,
            const SourceFrameEffects &Effects = {}) {
    add(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)});
    auto &Contract = Calls[*nativeSourceCallKey(Low.Blocks[0].Ops.back())];
    Contract.Signature = &Type;
    static_cast<SourceFrameEffects &>(Contract) = Effects;
  }
  void begin(uint64_t Offset = 0, uint64_t Flags = 33) {
    add(NdOp::COPY, X0, {X20});
    address(X1, Offset);
    add(NdOp::COPY, X2, {NdVar::scalar(Flags, 8)});
    add(NdOp::COPY, X3, {NdVar::scalar(0, 8)});
    call(Begin, Initialize);
  }
  void end(uint64_t Offset = 0) {
    address(X0, Offset);
    call(End, Finish);
  }
  void epilogue() {
    address(X9, 80);
    add(NdOp::LOAD, LR, {X9});
    add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(96, 8)});
    add(NdOp::RETURN, {}, {LR});
  }
  bool proves(const SourceFunctionTypeHint *Result = nullptr) const {
    return restoresNativeSourceState(Low, Arch::AArch64, Calls, nullptr,
                                     Result);
  }
};
} // namespace

TEST(SourceFrameAnalysis, RetainedScratchClosesNestedAndNonLifoLifetimes) {
  for (unsigned Flags : {0, 1, 32, 33})
    for (bool Reverse : {false, true}) {
      AccessLifetimeFixture F;
      F.begin(0, Flags);
      F.call(F.Ordinary);
      F.begin(32, Flags);
      F.end(Reverse ? 32 : 0);
      F.call(F.Ordinary);
      F.end(Reverse ? 0 : 32);
      F.epilogue();
      EXPECT_TRUE(F.proves()) << Flags << ": " << Reverse;
    }
}

TEST(SourceFrameAnalysis, RetainedScratchRejectsLostOrCorruptedLifetimes) {
  for (unsigned Case = 0; Case < 15; ++Case) {
    SCOPED_TRACE(Case);
    AccessLifetimeFixture F;
    if (Case != 0)
      F.begin();
    switch (Case) {
    case 0:
      F.end();
      break; // end before begin
    case 1:
      break; // missing end
    case 2:
      F.begin();
      F.end();
      break; // same live record twice
    case 3:
      F.end();
      F.end();
      break;
    case 4:
      F.end(8);
      break;
    case 5:
      F.begin(16);
      F.end(16);
      F.end();
      break; // overlapping records
    case 6:
    case 7:
      F.address(F.X9, Case == 6 ? 0 : 23);
      F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, 1)});
      F.end();
      break;
    case 8:
      F.add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)});
      F.add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(16, 8)});
      F.end();
      break;
    case 9:
      F.add(NdOp::COPY, SP, {F.X20});
      F.end();
      break;
    case 10:
      F.address(X0, 0);
      F.Low.Blocks[0].Ops.back().Output.Size = 4;
      F.call(F.End, F.Finish);
      break;
    case 11:
    case 12: {
      SourceFrameEffects Effect;
      if (Case == 11)
        Effect.WritableFrameParameters[0] = 24;
      else
        Effect.ReadOnlyFrameParameters[0] = 24;
      F.address(X0, 0);
      F.call(F.Borrow, Effect);
      F.end();
      break;
    }
    case 13: // an external record could acquire our link through TLS
      F.begin(32);
      F.Low.Blocks[0].Ops[F.Low.Blocks[0].Ops.size() - 4] =
          op(NdOp::COPY, F.X1, {F.X20}, F.Next - 16);
      F.end(32);
      F.end();
      break;
    case 14: // a second untracked initialization does not unlink a live record
      F.begin(0, 0);
      F.end();
      break;
    }
    F.epilogue();
    EXPECT_FALSE(F.proves());
  }
}

TEST(SourceFrameAnalysis, RetainedScratchCannotDisappearAtJoinsOrBackedges) {
  for (unsigned Case = 0; Case < 5; ++Case)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Case);
      AccessLifetimeFixture F;
      const size_t BeforeBegin = F.Low.Blocks[0].Ops.size();
      F.begin();
      const size_t AfterBegin = F.Low.Blocks[0].Ops.size();
      F.end();
      const size_t AfterEnd = F.Low.Blocks[0].Ops.size();
      F.epilogue();
      const auto Ops = F.Low.Blocks[0].Ops;
      F.Low.Blocks.resize(4);
      for (int I = 0; I < 4; ++I) {
        F.Low.Blocks[I] = {};
        F.Low.Blocks[I].Id = I;
        F.Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      }
      auto &Root = F.Low.Blocks[0], &Left = F.Low.Blocks[1],
           &Right = F.Low.Blocks[2], &Join = F.Low.Blocks[3];
      Root.Succs = {1, 2};
      Left.Preds = Right.Preds = {0};
      Left.Succs = Right.Succs = {3};
      Join.Preds = {1, 2};
      const auto Nop = op(NdOp::COPY, F.X9, {NdVar::scalar(0, 8)}, 0x1900);
      Left.Ops = Right.Ops = {Nop};
      if (Case < 2) {
        Root.Ops.assign(Ops.begin(), Ops.begin() + BeforeBegin);
        // A balanced access in one arm is fine. An unclosed access must not
        // disappear in the other arm's empty set at the common return.
        Left.Ops.assign(Ops.begin() + BeforeBegin,
                        Ops.begin() + (Case == 0 ? AfterEnd : AfterBegin));
        Join.Ops.assign(Ops.begin() + AfterEnd, Ops.end());
      } else {
        Root.Ops.assign(Ops.begin(), Ops.begin() + AfterBegin);
        Join.Ops.assign(Ops.begin() + AfterBegin, Ops.end());
        if (Case == 2)
          Left.Ops.assign(Ops.begin() + AfterBegin, Ops.begin() + AfterEnd);
        else {
          Left.Preds.push_back(1);
          Left.Succs.push_back(1);
          if (Case == 4)
            Left.Ops.assign(Ops.begin() + AfterBegin, Ops.begin() + AfterEnd);
        }
      }
      if (Reverse)
        std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_EQ(F.proves(), Case == 0 || Case == 3);
    }
}

TEST(SourceFrameAnalysis, OpaquePointerTaintSurvivesEndMayWritesAndFrameReuse) {
  auto Result = signature(0);
  Result.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Result.ReturnType = NdType::makeInt(8, false);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Result, Arch::AArch64, Error));
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    AccessLifetimeFixture F;
    F.begin();
    F.begin(32);
    F.end(32);
    F.end();
    if (Case == 1) {
      F.begin(0, 0);
      F.end();
    } // only may overwrite
    if (Case == 2) {
      F.add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(96, 8)});
      F.add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(96, 8)});
    }
    if (Case == 3 || Case == 4) {
      F.address(F.X9, 0);
      F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, Case == 3 ? 1 : 8)});
    }
    if (Case == 5) {
      for (unsigned I : {0, 8, 16}) {
        F.address(F.X9, I);
        F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, 8)});
      }
    }
    if (Case == 8 || Case == 9) {
      SourceFrameEffects Effect;
      if (Case == 8)
        Effect.ReadOnlyFrameParameters[0] = 24;
      else
        Effect.WritableFrameParameters[0] = 24;
      F.address(X0, 0);
      F.call(F.Borrow, Effect);
    }
    F.address(F.X9, 16);
    F.add(NdOp::LOAD, X0, {F.X9});
    if (Case == 6)
      F.add(NdOp::STORE, {}, {F.X20, X0});
    if (Case == 7)
      F.add(NdOp::LOAD, X0, {X0}); // inexact private alias
    if (Case == 10) {
      F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, 8)});
      // A subsequent exact overwrite cannot sanitize the prior loaded value.
    }
    if (Case == 11)
      F.add(NdOp::COPY, X0, {NdVar::scalar(7, 8)});
    F.epilogue();
    EXPECT_EQ(F.proves(&Result), Case == 5);
  }
}

TEST(SourceFrameAnalysis, PrefixQueryRequiresRetainedRecordsAlreadyClosed) {
  for (unsigned Flags : {0, 1, 32, 33})
    for (bool Closed : {false, true}) {
      FrameFixture F;
      AccessLifetimeFixture Protocol;
      const auto X1 = NdVar::reg(a64reg::X1, 8);
      F.prefix({op(NdOp::COPY, X0, {NdVar::scalar(1, 8)}, 0x1024),
                op(NdOp::COPY, X1, {SP}, 0x1028),
                op(NdOp::COPY, Protocol.X2, {NdVar::scalar(Flags, 8)}, 0x102c),
                op(NdOp::COPY, Protocol.X3, {NdVar::scalar(0, 8)}, 0x1030),
                op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1034)});
      auto Calls = callsAt(0x1034, Protocol.Begin, Protocol.Initialize);
      if (Closed) {
        // prefix() inserts at a fixed index; append end after the begin block.
        auto &Ops = F.Low.Blocks[0].Ops;
        Ops.insert(Ops.begin() + 11,
                   {op(NdOp::COPY, X0, {SP}, 0x1038),
                    op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x103c)});
        Calls.merge(callsAt(0x103c, Protocol.End, Protocol.Finish));
      }
      EXPECT_EQ(bool(F.query(Calls)), Closed || Flags < 32);
    }
}

namespace {
struct RecordCopyFixture {
  LowFunc Low;
  SourceFunctionTypeHint Entry, Producer, Transform, Consumer;
  NativeSourceCalls Calls;
  NativeSourceCallKey Site;
  const NdVar X1 = NdVar::reg(a64reg::X1, 8);
  const NdVar X2 = NdVar::reg(a64reg::X2, 8);
  const NdVar X8 = NdVar::reg(a64reg::X8, 8);
  const NdVar X9 = NdVar::reg(a64reg::X9, 8);
  const NdVar LR = NdVar::reg(a64reg::X30, 8);
  va_t Next = 0x2000;
  RecordCopyFixture() {
    Low.Entry = Next;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = Low.Entry;
    Low.Blocks.push_back(B);
    Entry = signature(0);
    Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Producer = signature(0);
    Producer.ReturnType =
        NdType::makeStruct(std::vector<TypeRef>(16, NdType::makeFloat(8)));
    std::string Error;
    EXPECT_TRUE(assignDarwinFixedSourceABI(Producer, Arch::AArch64, Error));
    Transform = Producer;
    Transform.Parameters = {{"input", NdType::makePtr(NdType::makeVoid())}};
    EXPECT_TRUE(assignDarwinFixedSourceABI(Transform, Arch::AArch64, Error));
    Consumer = signature(2);
    Consumer.Parameters.push_back({"record", Producer.ReturnType});
    EXPECT_TRUE(assignDarwinFixedSourceABI(Consumer, Arch::AArch64, Error));
    add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(288, 8)});
    address(X9, 272);
    add(NdOp::STORE, {}, {X9, LR});
    address(X8, 128);
    SourceFrameEffects Write;
    Write.InitializesIndirectResult = true;
    call(Producer, Write);
    address(X8, 0);
    address(X0, 128);
    Write.WritableFrameParameters[0] = 128;
    Write.InitializedFrameParameters.insert(0);
    call(Transform, Write);
    add(NdOp::COPY, X0, {NdVar::scalar(0, 8)});
    add(NdOp::COPY, X1, {NdVar::scalar(0x8000, 8)});
    address(X2, 0);
    SourceFrameEffects Copy;
    Copy.ByValueFrameParameters.insert(2);
    Site = call(Consumer, Copy);
    epilogue();
  }
  void add(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
    Low.Blocks[0].Ops.push_back(op(Opcode, Output, Inputs, Next));
    Next += 4;
  }
  void address(NdVar Output, unsigned Offset) {
    add(NdOp::INT_ADD, Output, {SP, NdVar::scalar(Offset, 8)});
  }
  NativeSourceCallKey call(const SourceFunctionTypeHint &ABI,
                           const SourceFrameEffects &Effects) {
    add(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)});
    const auto Key = *nativeSourceCallKey(Low.Blocks[0].Ops.back());
    Calls.merge(callsAt(Key.Instruction, ABI, Effects));
    return Key;
  }
  void epilogue() {
    address(X9, 272);
    add(NdOp::LOAD, LR, {X9});
    add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(288, 8)});
    add(NdOp::RETURN, {}, {LR});
  }
  auto query() const {
    return sourceFrameByValueCopies(Low, Arch::AArch64, Calls, Site, Entry);
  }
};
} // namespace

TEST(SourceFrameAnalysis, ProvesInitializedCopiesFromIndependentResultEffects) {
  RecordCopyFixture F;
  ASSERT_TRUE(F.query());
  EXPECT_EQ(*F.query(), (std::vector<SourceFrameByValueCopy>{{2, -288, 128}}));
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
  EXPECT_FALSE(
      sourceFrameByValueCopies(F.Low, Arch::X64, F.Calls, F.Site, F.Entry));
  auto Wrong = F.Site;
  ++Wrong.Sequence;
  EXPECT_FALSE(
      sourceFrameByValueCopies(F.Low, Arch::AArch64, F.Calls, Wrong, F.Entry));
  // An effect is independent evidence, not an implication of the return ABI.
  F.Calls.begin()->second.InitializesIndirectResult = false;
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, CopyEffectsKeepPointerAndRecordPermissionsDistinct) {
  RecordCopyFixture F;
  const auto Valid =
      static_cast<const SourceFrameEffects &>(F.Calls.at(F.Site));
  EXPECT_TRUE(sourceFrameEffectsMatchABI(Valid, F.Consumer));
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    auto ABI = F.Consumer;
    auto Effects = Valid;
    if (Case == 0)
      Effects.ByValueFrameParameters = {0};
    if (Case == 1)
      Effects.ByValueFrameParameters = {3};
    if (Case == 2)
      Effects.ReadOnlyFrameParameters[2] = 128;
    if (Case == 3)
      Effects.WritableFrameParameters[2] = 128;
    if (Case == 4)
      Effects.InitializedFrameParameters = {2};
    if (Case == 5)
      Effects.InitializesIndirectResult = true;
    if (Case == 6)
      ABI.Parameters[2].Location.ValueBytes = 4;
    if (Case == 7)
      ABI.Parameters[2].Type = NdType::makePtr(NdType::makeVoid());
    if (Case == 8)
      ABI.Parameters[2].IndirectByValue = false;
    if (Case == 9) {
      ABI.Parameters[2].Location.Kind = SourceABICarrierKind::Stack;
      ABI.Parameters[2].Location.RegisterOffset = 0;
    }
    EXPECT_FALSE(sourceFrameEffectsMatchABI(Effects, ABI));
  }
}

TEST(SourceFrameAnalysis, RejectsPartialExpiredAliasedAndEscapingCopyStorage) {
  for (unsigned Case = 0; Case < 14; ++Case) {
    SCOPED_TRACE(Case);
    RecordCopyFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto At = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
      return nativeSourceCallKey(Op) == F.Site;
    });
    const auto Before = At - Ops.begin();
    std::vector<LowOp> Prefix;
    if (Case < 4) {
      Prefix.push_back(op(NdOp::INT_ADD, F.X2,
                          {SP, NdVar::scalar(Case == 0   ? 1
                                             : Case == 1 ? 8
                                             : Case == 2 ? 192
                                                         : 320,
                                             8)},
                          0x3000));
    } else if (Case == 4) {
      Prefix.push_back(
          op(NdOp::COPY, F.X2, {NdVar::scalar(0x9000, 8)}, 0x3000));
    } else if (Case == 5) {
      Prefix.push_back(op(NdOp::COPY, NdVar::reg(a64reg::X2, 4),
                          {NdVar::scalar(0, 4)}, 0x3000));
    } else if (Case == 6 || Case == 7) {
      Prefix.push_back(
          op(NdOp::STORE, {}, {Case == 6 ? F.X1 : F.X2, SP}, 0x3000));
    } else if (Case == 8) {
      Prefix.push_back(
          op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)}, 0x3000));
      Prefix.push_back(
          op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(16, 8)}, 0x3004));
    } else if (Case == 9 || Case == 10) {
      // Another argument can observe an alias even with a bounded borrow.
      F.Calls.at(F.Site).ReadOnlyFrameParameters[0] = 8;
      Prefix.push_back(op(NdOp::INT_ADD, X0,
                          {SP, NdVar::scalar(Case == 9 ? 0 : 120, 8)}, 0x3000));
    } else if (Case == 11) {
      F.Calls.at(F.Site).ByValueFrameParameters.clear();
    } else if (Case == 12) {
      // One definite word cannot replace a complete 128-byte producer.
      Ops[4] = op(NdOp::STORE, {}, {F.X8, NdVar::scalar(0, 8)}, 0x2010);
    } else {
      Prefix.push_back(op(NdOp::CALL, X0, {NdVar::cst(0x9000, 8)}, 0x3000));
    }
    Ops.insert(Ops.begin() + Before, Prefix.begin(), Prefix.end());
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, ConsumedCopiesNeedNewWritesBeforeLaterReads) {
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    RecordCopyFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto At = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
      return nativeSourceCallKey(Op) == F.Site;
    });
    std::vector<LowOp> After{op(NdOp::COPY, F.X9, {SP}, 0x3000)};
    if (Case == 1 || Case == 2)
      After.push_back(op(NdOp::STORE, {},
                         {F.X9, NdVar::scalar(5, Case == 1 ? 4 : 8)}, 0x3004));
    if (Case != 3)
      After.push_back(op(NdOp::LOAD, X0, {F.X9}, 0x3008));
    if (Case == 4)
      After.push_back(op(NdOp::STORE, {}, {F.X9, NdVar::scalar(5, 8)}, 0x300c));
    Ops.insert(At + 1, After.begin(), After.end());
    EXPECT_EQ(bool(F.query()), Case == 2 || Case == 3);
  }
}

TEST(SourceFrameAnalysis, DefiniteResultsRespectSavedStateAndActiveExtent) {
  for (unsigned Offset : {1, 192, 272, 288}) {
    RecordCopyFixture F;
    F.Low.Blocks[0].Ops[3].Inputs[1] = NdVar::scalar(Offset, 8);
    EXPECT_FALSE(F.query()) << Offset;
  }
}

TEST(SourceFrameAnalysis, CopyInitializationIsAMustFactAcrossEveryPredecessor) {
  for (unsigned Case = 0; Case < 6; ++Case)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Case);
      RecordCopyFixture F;
      const auto Original = F.Low.Blocks[0].Ops;
      F.Low.Blocks.resize(4);
      auto &Root = F.Low.Blocks[0], &Left = F.Low.Blocks[1],
           &Right = F.Low.Blocks[2], &Join = F.Low.Blocks[3];
      for (unsigned I = 0; I < 4; ++I) {
        F.Low.Blocks[I].Id = I;
        F.Low.Blocks[I].StartAddr = 0x2000 + I * 0x100;
      }
      Root.Ops.assign(Original.begin(), Original.begin() + 8);
      Root.Succs = {1, 2};
      Left.Preds = Right.Preds = {0};
      Left.Succs = Right.Succs = {3};
      Join.Preds = {1, 2};
      Join.Ops.assign(Original.begin() + 8, Original.end());
      Left.Ops =
          Right.Ops = {op(NdOp::COPY, F.X9, {NdVar::scalar(0, 8)}, 0x3000)};
      auto Borrow = signature(1);
      if (Case) {
        SourceFrameEffects Effects;
        if (Case == 1)
          Effects.ReadOnlyFrameParameters[0] = 8;
        else
          Effects.WritableFrameParameters[0] = 8;
        Left.Ops = {op(NdOp::INT_ADD, X0,
                       {SP, NdVar::scalar(Case == 5 ? 128 : 0, 8)}, 0x3000),
                    op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x3004)};
        F.Calls.merge(callsAt(0x3004, Borrow, Effects));
        if (Case == 3 || Case == 4) {
          Left.Ops.push_back(op(NdOp::COPY, F.X9, {SP}, 0x3008));
          Left.Ops.push_back(op(NdOp::STORE, {},
                                {F.X9, NdVar::scalar(7, Case == 3 ? 8 : 4)},
                                0x300c));
        }
      }
      if (Reverse)
        std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_EQ(bool(F.query()), Case != 2 && Case != 4);
    }
}

TEST(SourceFrameAnalysis,
     CopyInitializationIncludesConsumptionOnLoopBackedges) {
  for (bool Reinitialize : {false, true})
    for (bool Reverse : {false, true}) {
      RecordCopyFixture F;
      const auto Original = F.Low.Blocks[0].Ops;
      F.Low.Blocks.resize(3);
      auto &Root = F.Low.Blocks[0], &Loop = F.Low.Blocks[1],
           &Exit = F.Low.Blocks[2];
      for (unsigned I = 0; I < 3; ++I) {
        F.Low.Blocks[I].Id = I;
        F.Low.Blocks[I].StartAddr = 0x2000 + I * 0x100;
      }
      Root.Ops.assign(Original.begin(), Original.begin() + 8);
      Root.Succs = {1};
      Loop.Preds = {0, 1};
      Loop.Succs = {1, 2};
      Loop.Ops.assign(Original.begin() + 8, Original.begin() + 12);
      if (Reinitialize) {
        std::vector<LowOp> Prefix(Original.begin() + 3, Original.begin() + 8);
        for (size_t I = 0; I < Prefix.size(); ++I) {
          const auto OldSite = nativeSourceCallKey(Prefix[I]);
          Prefix[I].Addr = 0x3000 + I * 4;
          if (OldSite)
            F.Calls.emplace(*nativeSourceCallKey(Prefix[I]),
                            F.Calls.at(*OldSite));
        }
        Loop.Ops.insert(Loop.Ops.begin(), Prefix.begin(), Prefix.end());
      }
      Exit.Preds = {1};
      Exit.Ops.assign(Original.begin() + 12, Original.end());
      if (Reverse)
        std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_EQ(bool(F.query()), Reinitialize);
    }
}

TEST(SourceFrameAnalysis,
     CopyAndResultEffectsPreserveRetainedScratchLifetimes) {
  for (unsigned ScratchOffset : {0, 128, 288}) {
    RecordCopyFixture F;
    AccessLifetimeFixture Protocol;
    auto &Ops = F.Low.Blocks[0].Ops;
    for (auto &Op : Ops) {
      if (Op.Output == SP && Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Offset == 288)
        Op.Inputs[1] = NdVar::scalar(384, 8);
      if (Op.Output == F.X9 && Op.Opcode == NdOp::INT_ADD &&
          Op.Inputs[1].Offset == 272)
        Op.Inputs[1] = NdVar::scalar(368, 8);
    }
    Ops.insert(
        Ops.begin() + 3,
        {op(NdOp::COPY, X0, {NdVar::scalar(0x6000, 8)}, 0x3100),
         op(NdOp::INT_ADD, F.X1, {SP, NdVar::scalar(ScratchOffset, 8)}, 0x3104),
         op(NdOp::COPY, F.X2, {NdVar::scalar(32, 8)}, 0x3108),
         op(NdOp::COPY, Protocol.X3, {NdVar::scalar(0, 8)}, 0x310c),
         op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x3110)});
    F.Calls.merge(callsAt(0x3110, Protocol.Begin, Protocol.Initialize));
    auto After = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
      return nativeSourceCallKey(Op) == F.Site;
    });
    Ops.insert(
        After + 1,
        {op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(ScratchOffset, 8)}, 0x3120),
         op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x3124)});
    F.Calls.merge(callsAt(0x3124, Protocol.End, Protocol.Finish));
    EXPECT_EQ(bool(F.query()), ScratchOffset == 288);
  }
}

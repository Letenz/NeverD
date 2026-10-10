//===- MedNoReturnTests.cpp - Internal no-return propagation tests -------===//

#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedNoReturn.h"
#include "neverd/lift/X86Regs.h"

#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

MedOp trapOp() {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  return Op;
}

MedOp callOp(va_t Target) {
  MedOp Op;
  Op.Opcode = NdOp::CALL;
  Op.addInput(MedVar::makeConst(Target, 8));
  return Op;
}

MedOp returnOp() {
  MedOp Op;
  Op.Opcode = NdOp::RETURN;
  return Op;
}

MedBlock block(int Id, std::initializer_list<MedOp> Ops,
               std::initializer_list<int> Succs = {}) {
  MedBlock Block;
  Block.Id = Id;
  Block.Ops.assign(Ops.begin(), Ops.end());
  Block.Succs.assign(Succs.begin(), Succs.end());
  return Block;
}

MedFunc function(va_t Entry, std::initializer_list<MedBlock> Blocks) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "sub_" + std::to_string(Entry);
  Func.Blocks.assign(Blocks.begin(), Blocks.end());
  return Func;
}

} // namespace

TEST(MedNoReturn, PropagatesOnlyFromExplicitTerminatingFacts) {
  constexpr va_t TrapEntry = 0x1000;
  constexpr va_t WrapperEntry = 0x2000;
  constexpr va_t MixedEntry = 0x3000;
  constexpr va_t UnknownEntry = 0x4000;

  std::vector<MedFunc> Funcs;
  // Put the wrapper first so proving it requires a second fixed-point round.
  Funcs.push_back(
      function(WrapperEntry, {block(0, {callOp(TrapEntry), returnOp()})}));
  Funcs.push_back(function(MixedEntry, {block(0, {}, {1, 2}),
                                        block(1, {callOp(TrapEntry)}, {2}),
                                        block(2, {returnOp()})}));
  Funcs.push_back(function(UnknownEntry, {block(0, {MedOp{}})}));
  Funcs.push_back(function(TrapEntry, {block(0, {trapOp()})}));

  propagateInternalNoReturn(Funcs, Arch::AArch64);

  EXPECT_TRUE(Funcs[0].DoesNotReturn);
  EXPECT_FALSE(Funcs[1].DoesNotReturn);
  EXPECT_FALSE(Funcs[2].DoesNotReturn);
  EXPECT_TRUE(Funcs[3].DoesNotReturn);
  EXPECT_TRUE(Funcs[0].Blocks[0].Ops[0].DoesNotReturn);
  EXPECT_TRUE(Funcs[1].Blocks[1].Ops[0].DoesNotReturn);

  // The late pipeline refresh is intentionally idempotent.
  propagateInternalNoReturn(Funcs, Arch::AArch64);
  EXPECT_TRUE(Funcs[0].DoesNotReturn);
  EXPECT_TRUE(Funcs[1].Blocks[1].Ops[0].DoesNotReturn);
}

TEST(MedNoReturn, NativeSourceEffectsFollowCurrentProofWithoutSharedMutation) {
  auto Call = callOp(0x2000);
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::Native;
  Hint->TargetAddress = 0x2000;
  Call.SourceCallHint = Hint;
  std::vector<MedFunc> Functions = {
      function(0x1000, {block(0, {Call, returnOp()})}),
      function(0x2000, {block(0, {trapOp()})})};
  propagateInternalNoReturn(Functions, Arch::AArch64);
  EXPECT_TRUE(Functions[0].Blocks[0].Ops[0].SourceCallHint->DoesNotReturn);
  EXPECT_FALSE(Hint->DoesNotReturn);
  EXPECT_TRUE(hasProvenNoReturnExit(Functions[1], Arch::AArch64));
  Functions[1].Blocks[0].Ops = {returnOp()};
  EXPECT_FALSE(hasProvenNoReturnExit(Functions[1], Arch::AArch64));
  propagateInternalNoReturn(Functions, Arch::AArch64);
  EXPECT_FALSE(Functions[0].DoesNotReturn);
  EXPECT_FALSE(Functions[0].Blocks[0].Ops[0].SourceCallHint->DoesNotReturn);
}

TEST(MedNoReturn, RuntimeImportEffectSurvivesAnInventoriedVeneer) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (const auto Kind : {SourceCallTypeHint::Kind::ObjCRuntimeCall,
                            SourceCallTypeHint::Kind::DarwinRuntimeCall,
                            SourceCallTypeHint::Kind::SwiftRuntimeCall}) {
      SCOPED_TRACE(static_cast<int>(Kind));
      auto Call = callOp(0x2000);
      Call.DoesNotReturn = true;
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = Kind;
      Hint->TargetAddress = 0x4000; // The import slot, not the veneer entry.
      Hint->DoesNotReturn = true;
      Hint->Signature.ReturnType = NdType::makeVoid();
      std::string Diagnostic;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture,
                                              Diagnostic))
          << Diagnostic;
      Call.SourceCallHint = Hint;
      // The imported entry has no local machine termination proof. A generic
      // inventory can still contain its veneer alongside callers and wrappers.
      std::vector<MedFunc> Functions = {
          function(0x1000, {block(0, {Call, returnOp()})}),
          function(0x2000, {block(0, {MedOp{}})}),
          function(0x3000, {block(0, {callOp(0x1000), returnOp()})})};
      for (unsigned Iteration = 0; Iteration < 2; ++Iteration) {
        propagateInternalNoReturn(Functions, Architecture);
        EXPECT_TRUE(Functions[0].DoesNotReturn);
        EXPECT_TRUE(Functions[0].Blocks[0].Ops[0].DoesNotReturn);
        EXPECT_FALSE(Functions[1].DoesNotReturn);
        EXPECT_TRUE(Functions[2].DoesNotReturn);
      }

      // A source declaration is not an independent machine termination fact.
      Functions[0].Blocks[0].Ops[0].DoesNotReturn = false;
      propagateInternalNoReturn(Functions, Architecture);
      EXPECT_FALSE(Functions[0].DoesNotReturn);
      EXPECT_FALSE(Functions[2].DoesNotReturn);

      // Revoking the external source effect must also revoke the old marker.
      Functions[0].Blocks[0].Ops[0].DoesNotReturn = true;
      Hint->DoesNotReturn = false;
      propagateInternalNoReturn(Functions, Architecture);
      EXPECT_FALSE(Functions[0].DoesNotReturn);
      EXPECT_FALSE(Functions[2].DoesNotReturn);

      // A malformed binding cannot protect a stale internal marker either.
      Functions[0].Blocks[0].Ops[0].DoesNotReturn = true;
      Hint->DoesNotReturn = true;
      Hint->Signature.Parameters = {{"missing", NdType::makeInt(8, false)}};
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint->Signature, Architecture,
                                              Diagnostic))
          << Diagnostic;
      propagateInternalNoReturn(Functions, Architecture);
      EXPECT_FALSE(Functions[0].DoesNotReturn);
      EXPECT_FALSE(Functions[2].DoesNotReturn);
    }
  }
}

TEST(MedNoReturn, NoReturnCallStillFollowsExceptionalContinuations) {
  for (const auto Kind :
       {ExceptionalEdgeKind::CxxCatch, ExceptionalEdgeKind::SEHHandler,
        ExceptionalEdgeKind::ItaniumCatchPad, ExceptionalEdgeKind::GoRecover}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    for (bool Indirect : {false, true}) {
      SCOPED_TRACE(Indirect);
      for (unsigned Destination = 0; Destination != 4; ++Destination) {
        SCOPED_TRACE(Destination);
        auto Call = callOp(0x2000);
        if (Indirect) {
          Call.Opcode = NdOp::INDIR_CALL;
          Call.DoesNotReturn = true;
        }
        auto Entry = block(0, {Call}, {2});
        // 0: a handler returns; 1: it also terminates; 2: outside this
        // body; 3: a missing block. Only destination 1 proves no return.
        Entry.ExceptionalSuccs.push_back({Destination == 2   ? -1
                                          : Destination == 3 ? 99
                                                             : 1,
                                          0x1010, Kind});
        std::vector<MedFunc> Functions = {
            function(0x1000,
                     {Entry,
                      block(1, {Destination == 1 ? trapOp() : returnOp()}),
                      block(2, {returnOp()})}),
            function(0x2000, {block(0, {trapOp()})}),
            function(0x3000, {block(0, {callOp(0x1000), returnOp()})})};
        for (unsigned Refresh = 0; Refresh != 2; ++Refresh) {
          propagateInternalNoReturn(Functions, Arch::AArch64);
          EXPECT_EQ(Functions[0].DoesNotReturn, Destination == 1);
          EXPECT_TRUE(Functions[0].Blocks[0].Ops[0].DoesNotReturn);
          EXPECT_EQ(Functions[2].DoesNotReturn, Destination == 1);
          EXPECT_TRUE(Functions[1].DoesNotReturn);
        }
      }
    }
  }
}

TEST(MedNoReturn, EarlierExceptionalPathSurvivesALaterTerminatingCall) {
  auto Entry = block(0, {}, {1});
  Entry.ExceptionalSuccs.push_back({2, 0x1020, ExceptionalEdgeKind::CxxCatch});
  auto Call = callOp(0x2000);
  Call.DoesNotReturn = true;
  const auto Function =
      function(0x1000, {Entry, block(1, {Call}), block(2, {returnOp()})});
  EXPECT_FALSE(hasProvenNoReturnExit(Function, Arch::X86));
}

TEST(MedNoReturn, CallFollowedByInt3DoesNotProveNoreturn) {
  constexpr va_t Entry = 0x140001000;
  constexpr va_t Helper = 0x140002000;
  MedOp Int3;
  Int3.Opcode = NdOp::INTRINSIC;
  Int3.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Int3), 2));
  std::vector<MedFunc> Funcs;
  Funcs.push_back(
      function(Entry, {block(0, {callOp(Helper), Int3, returnOp()})}));

  propagateInternalNoReturn(Funcs, Arch::X64);

  ASSERT_FALSE(Funcs[0].Blocks.empty());
  ASSERT_FALSE(Funcs[0].Blocks[0].Ops.empty());
  EXPECT_FALSE(Funcs[0].DoesNotReturn);
  EXPECT_FALSE(Funcs[0].Blocks[0].Ops[0].DoesNotReturn);
}

TEST(MedNoReturn, CallFollowedByTrapDoesNotInventCalleeNoReturn) {
  constexpr va_t Entry = 0x1000;
  constexpr va_t Helper = 0x2000;
  constexpr va_t OrdinaryCaller = 0x3000;
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    for (Intrinsic Kind : {Intrinsic::Int3, Intrinsic::Int1, Intrinsic::Ud2}) {
      SCOPED_TRACE(static_cast<int>(Kind));
      MedOp Trap;
      Trap.Opcode = NdOp::INTRINSIC;
      Trap.addInput(MedVar::makeConst(static_cast<uint64_t>(Kind), 2));
      auto Call = callOp(Helper);
      auto Hint = std::make_shared<SourceCallTypeHint>();
      Hint->CallKind = SourceCallTypeHint::Kind::Native;
      Hint->TargetAddress = Helper;
      Call.SourceCallHint = Hint;
      std::vector<MedFunc> Functions = {
          function(Entry, {block(0, {Call, Trap, returnOp()})}),
          function(Helper, {block(0, {returnOp()})}),
          function(OrdinaryCaller, {block(0, {callOp(Helper), returnOp()})})};
      for (unsigned Iteration = 0; Iteration < 2; ++Iteration) {
        propagateInternalNoReturn(Functions, Architecture);
        EXPECT_FALSE(Functions[0].Blocks[0].Ops[0].DoesNotReturn);
        ASSERT_TRUE(Functions[0].Blocks[0].Ops[0].SourceCallHint);
        EXPECT_FALSE(
            Functions[0].Blocks[0].Ops[0].SourceCallHint->DoesNotReturn);
        EXPECT_FALSE(Hint->DoesNotReturn);
        EXPECT_FALSE(Functions[1].DoesNotReturn);
        EXPECT_FALSE(Functions[2].DoesNotReturn);
        EXPECT_FALSE(Functions[2].Blocks[0].Ops[0].DoesNotReturn);
      }
    }
  }
}

TEST(RegistrationCallNoReturn, LowToMedRequiresCurrentCallIdentityAndTarget) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    LowFunc Low;
    Low.Entry = 0x1000;
    Low.Name = "checked_source_call";
    Low.Blocks.resize(2);
    auto &B = Low.Blocks[0];
    B.Id = 0;
    B.StartAddr = 0x1000;
    B.EndAddr = 0x1005;
    B.Succs = {1};
    LowInstructionBoundary Boundary;
    Boundary.Address = 0x1000;
    Boundary.Size = 5;
    Boundary.Control = LowInstructionControl::Call;
    Boundary.FirstOp = 0;
    Boundary.OpCount = 1;
    B.InstructionBoundaries.push_back(Boundary);
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1000;
    Call.Seq = 4;
    Call.Output = NdVar::reg(x86reg::RAX, 4);
    Call.addInput(NdVar::cst(0x2100, 4));
    B.Ops.push_back(Call);
    Low.Blocks[1].Id = 1;
    Low.Blocks[1].StartAddr = 0x1005;
    Low.Blocks[1].EndAddr = 0x1006;
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = 0x1005;
    Return.Seq = 0;
    Return.addInput(NdVar::cst(7, 4));
    Low.Blocks[1].Ops.push_back(Return);
    Low.RegistrationStates.emplace();
    RegistrationCallFrameEffect Effect;
    Effect.Address = 0x1000;
    Effect.EndAddress = 0x1005;
    Effect.OpSeq = 4;
    Effect.Target = 0x2100;
    Effect.DoesNotReturn = true;
    Low.RegistrationStates->CallFrameEffects.push_back(Effect);
    switch (Mutation) {
    case 1:
      B.Ops[0].Inputs[0] = NdVar::cst(0x2200, 4);
      break;
    case 2:
      B.Ops[0].Addr = 0x1001;
      break;
    case 3:
      B.Ops[0].Seq = 5;
      break;
    case 4:
      B.Ops[0].Inputs[0] = NdVar::cst(0x2100, 8);
      break;
    case 5:
      B.Ops[0].Opcode = NdOp::INDIR_CALL;
      break;
    }
    auto Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
    unsigned Calls = 0;
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          ++Calls;
          EXPECT_EQ(Op.DoesNotReturn, Mutation == 0) << Mutation;
        }
    EXPECT_EQ(Calls, 1u) << Mutation;
  }
}

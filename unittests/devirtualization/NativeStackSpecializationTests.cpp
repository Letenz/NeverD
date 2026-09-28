//===- NativeStackSpecializationTests.cpp - Physical stack control --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/Support/Errc.h"

#include <map>
#include <optional>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar c(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (NdVar Input : Inputs)
    Result.addInput(Input);
  return Result;
}
LowOp branch(va_t Target) {
  return operation(NdOp::BRANCH, {}, {NdVar::cst(Target, 8)});
}
LowOp call(va_t Target) {
  return operation(NdOp::CALL, r(0), {NdVar::cst(Target, 8)});
}
LowOp ret() { return operation(NdOp::RETURN, {}, {r(0)}); }

class StackProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;

  void add(va_t Address, std::initializer_list<LowOp> Ops,
           SpecializationNativeStackControl Native =
               SpecializationNativeStackControl::None) {
    SpecializationInstruction Insn;
    Insn.Ops = Ops;
    Insn.Origin.Address = Address;
    Insn.Origin.Size = 1;
    Insn.Origin.OpCount = Ops.size();
    Insn.Fallthrough = {Address + 1};
    Insn.NativeStackControl = Native;
    for (size_t I = 0; I < Insn.Ops.size(); ++I) {
      LowOp &Op = Insn.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::CALL) {
        Insn.Origin.Control = LowInstructionControl::Call;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Call;
        Insn.Origin.Immediate = Op.Inputs[0].Offset;
      } else if (Op.Opcode == NdOp::RETURN) {
        Insn.Origin.Control = LowInstructionControl::Return;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
        Insn.Origin.Control = LowInstructionControl::Branch;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          Insn.Origin.ControlFlags |= LowInstructionControlFlag::Conditional;
        Insn.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code[Address] = std::move(Insn);
  }

  void nativeCall(va_t Address, va_t Target) {
    add(Address, {call(Target)}, SpecializationNativeStackControl::Call);
  }
  void nativeReturn(va_t Address) {
    add(Address, {ret()}, SpecializationNativeStackControl::Return);
  }
  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto At = Code.find(Cursor.Address);
    if (At == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "unreachable continuation was visited");
    return At->second;
  }
};

SpecializationOptions stackOptions() {
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.RequireRestoredFrameAtReturn = true;
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  return Options;
}

struct Execution {
  uint64_t Value;
  uint64_t Stack;
};

std::optional<Execution> execute(const LowFunc &Function, uint64_t Input = 0,
                                 uint64_t Flag = 0) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, 0, Ctx.mkConst(64, 0));
  State.write(SymSpace::Register, 8, Ctx.mkConst(64, Input));
  State.write(SymSpace::Register, 32, Ctx.mkConst(64, 0x10000));
  State.write(SymSpace::Register, x86reg::CF, Ctx.mkConst(8, Flag));
  va_t Address = Function.Entry;
  for (unsigned Step = 0; Step < 1000; ++Step) {
    const LowBlock *Block = nullptr;
    for (const LowBlock &Candidate : Function.Blocks)
      if (Candidate.StartAddr == Address)
        Block = &Candidate;
    if (!Block)
      return std::nullopt;
    SymExec Exec(Ctx, State);
    bool Transferred = false;
    for (const LowOp &Op : Block->Ops) {
      const StepResult Flow = Exec.step(Op);
      if (Flow == StepResult::Continue)
        continue;
      if (Flow == StepResult::Return) {
        const auto Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
        const auto Stack = Ctx.asConst(State.read(SymSpace::Register, 32, 8));
        if (!Value || !Stack)
          return std::nullopt;
        return Execution{Value->getZExtValue(), Stack->getZExtValue()};
      }
      if (Flow != StepResult::Branch)
        return std::nullopt;
      const auto Target = Ctx.asConst(Exec.branchTarget());
      if (!Target)
        return std::nullopt;
      Address = Target->getZExtValue();
      Transferred = true;
      break;
    }
    if (!Transferred)
      return std::nullopt;
  }
  return std::nullopt;
}

TEST(NativeStackSpecialization, DirectCalleeRetainsPhysicalRegisterEffects) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::COPY, r(0), {c(17)})});
  P.nativeCall(0x101, 0x200);
  P.nativeReturn(0x102);
  P.add(0x200, {operation(NdOp::INT_ADD, r(0), {r(0), r(8)})});
  P.nativeReturn(0x201);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{19}, UINT64_MAX}) {
    auto Run = execute(Result.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
}

TEST(NativeStackSpecialization, SharedCalleeKeepsDistinctReturnContexts) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::COPY, r(0), {c(1)})});
  P.nativeCall(0x101, 0x200);
  P.nativeCall(0x102, 0x200);
  P.nativeReturn(0x103);
  P.nativeCall(0x200, 0x300);
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::INT_ADD, r(0), {r(0), c(3)})});
  P.nativeReturn(0x301);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 7u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, RestoredEntryStackMayDiscardCalleeFrames) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(40), r(32)})});
  P.nativeCall(0x101, 0x200);
  // There is deliberately no instruction at the ordinary continuation.
  P.add(0x200, {operation(NdOp::INT_SUB, r(40), {r(32), c(8)}),
                operation(NdOp::LOAD, r(32), {r(40)}),
                operation(NdOp::COPY, r(0), {c(53)}), branch(0x210)});
  P.nativeReturn(0x210);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 53u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, NativeReturnUsesActualStoredTarget) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  P.add(0x200, {operation(NdOp::STORE, {}, {r(32), c(0x300)})});
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::COPY, r(0), {c(71)})});
  P.nativeReturn(0x301);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 71u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, UnknownStoreDoesNotPreserveInternalReturnSlot) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  P.nativeReturn(0x101);
  P.add(0x200, {operation(NdOp::STORE, {}, {r(8), c(7)})});
  P.nativeReturn(0x201);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_FALSE(Result.complete());
  EXPECT_NE(Result.Diagnostic.find("native return"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, UncertifiedCallsStillFailClosed) {
  StackProvider P;
  P.add(0x100, {call(0x200)});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, RecursiveStackGrowthIsBounded) {
  StackProvider P;
  P.nativeCall(0x100, 0x100);
  auto Options = stackOptions();
  Options.MaxNativeReturnSlots = 4;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_NE(Result.Diagnostic.find("return-slot context budget"),
            std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, CallNextPushAndPopRetainPhysicalValue) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(32), {r(32), c(8)}),
                operation(NdOp::STORE, {}, {r(32), c(0x101)})});
  P.add(0x101, {operation(NdOp::LOAD, r(0), {r(32)}),
                operation(NdOp::INT_ADD, r(32), {r(32), c(8)})});
  P.nativeReturn(0x102);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 0x101u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, AffineFrameSpillSurvivesProjection) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}),
                operation(NdOp::LOAD, r(0), {r(40)})});
  P.nativeReturn(0x111);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 29u);
}

TEST(NativeStackSpecialization, PartialStoreInvalidatesWholeAffineSpill) {
  for (uint64_t Byte : {0u, 7u}) {
    StackProvider P;
    P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                  operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                  operation(NdOp::STORE, {}, {r(48), r(40)}), branch(0x110)});
    P.add(0x110, {operation(NdOp::INT_ADD, r(56), {r(48), c(Byte)}),
                  operation(NdOp::STORE, {}, {r(56), c(0, 1)}), branch(0x120)});
    P.add(0x120, {operation(NdOp::LOAD, r(56), {r(48)}),
                  operation(NdOp::STORE, {}, {r(56), c(29)}), ret()});
    auto Result = specializeInterpreter(P, {0x100}, stackOptions());
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, UnknownStoreInvalidatesAffineSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}),
                operation(NdOp::STORE, {}, {r(8), c(0)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, ConditionalOverwriteInvalidatesJoinedSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}),
                operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(8)})});
  P.add(0x101, {operation(NdOp::STORE, {}, {r(48), c(0, 1)}), branch(0x300)});
  P.add(0x200, {branch(0x300)});
  P.add(0x300, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, AdjacentStorePreservesWholeAffineSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::INT_ADD, r(56), {r(48), c(8)}),
                operation(NdOp::STORE, {}, {r(56), c(5)}), branch(0x120)});
  P.add(0x120, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}),
                operation(NdOp::LOAD, r(0), {r(40)})});
  P.nativeReturn(0x121);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 29u);
}

TEST(NativeStackSpecialization, MismatchedNativeControlCertificateIsRejected) {
  StackProvider P;
  P.add(0x100, {branch(0x200)}, SpecializationNativeStackControl::Call);
  auto Call = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Call.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Call.Residual.Blocks.empty());
  P.nativeReturn(0x100);
  P.Code[0x100].Origin.Immediate = 8;
  auto Return = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Return.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Return.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, EntryFlagsRequireExplicitMachineState) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_ZEXT, r(0), {r(x86reg::CF, 1)}), ret()});
  auto Options = stackOptions();
  auto Ordinary = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Ordinary.Status, SpecializationStatus::Unsupported);
  Options.ExplicitMachineState = true;
  auto Machine = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Machine.complete()) << Machine.Diagnostic;
  for (uint64_t Flag : {0u, 1u}) {
    auto Run = execute(Machine.Residual, 0, Flag);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Flag);
  }
}

TEST(NativeStackSpecialization, MachinePreconditionsRequireMachineInterface) {
  StackProvider P;
  P.add(0x100, {ret()});
  for (bool Cet : {false, true}) {
    auto Options = stackOptions();
    Options.X64CetDisabled = Cet;
    Options.NormalNonfaultingExecution = !Cet;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

} // namespace

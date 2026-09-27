//===- InterpreterSpecializationTests.cpp - Partial evaluation contracts
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/symbolic/SymExec.h"

#include <map>
#include <optional>
#include <set>
#include <utility>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

NdVar reg(uint64_t Offset, uint16_t Size = 8) {
  return NdVar::reg(Offset, Size);
}
NdVar constant(uint64_t Value, uint16_t Size = 8) {
  return NdVar::scalar(Value, Size);
}
LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp jump(va_t Address) { return op(NdOp::BRANCH, {}, {constant(Address)}); }
LowOp ret() { return op(NdOp::RETURN, {}, {}); }

class Provider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  std::map<va_t, uint8_t> Image;
  bool MalformedRead = false;

  void add(va_t Address, std::initializer_list<LowOp> Ops,
           va_t Fallthrough = InvalidVA) {
    SpecializationInstruction Instruction;
    Instruction.Ops = Ops;
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = 1;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Fallthrough == InvalidVA ? Address + 1
                                                        : Fallthrough};
    for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
      auto &Op = Instruction.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR) {
        Instruction.Origin.Control = LowInstructionControl::Branch;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Conditional;
        if (Op.Opcode == NdOp::INDIR_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Indirect;
        else
          Instruction.Origin.Immediate = Op.Inputs[0].Offset;
      } else if (Op.Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::CALL) {
        Instruction.Origin.Control = LowInstructionControl::Call;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Call;
        Instruction.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code[Address] = std::move(Instruction);
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto Found = Code.find(Cursor.Address);
    if (Found == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing test instruction");
    return Found->second;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Size) override {
    SpecializationImmutableRead Read;
    Read.Evidence = "test immutable allocation";
    for (unsigned I = 0; I < Size; ++I) {
      const auto Found = Image.find(Address + I);
      if (Found == Image.end())
        return std::nullopt;
      Read.Bytes.push_back(Found->second);
    }
    if (MalformedRead)
      Read.Bytes.clear();
    return Read;
  }
};

size_t count(const LowFunc &Function, NdOp Opcode) {
  size_t Count = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      Count += Op.Opcode == Opcode;
  return Count;
}

/// Independent concrete oracle for the small integer corpus. It intentionally
/// does not use SymExec: it executes residual operands, branches and byte
/// lanes.
std::optional<uint64_t> execute(const LowFunc &Function,
                                std::map<uint64_t, uint64_t> Inputs = {},
                                std::map<uint64_t, uint8_t> Memory = {}) {
  using Location = std::pair<VnodeSpace, uint64_t>;
  std::map<Location, uint8_t> Bytes;
  const auto write = [&](NdVar Destination, uint64_t Value) {
    for (unsigned I = 0; I < Destination.Size; ++I)
      Bytes[{Destination.Space, Destination.Offset + I}] =
          static_cast<uint8_t>(Value >> (I * 8));
  };
  const auto read = [&](NdVar Value) {
    uint64_t Result = Value.isConst() ? Value.Offset : 0;
    if (!Value.isConst())
      for (unsigned I = 0; I < Value.Size; ++I)
        Result |= uint64_t{Bytes[{Value.Space, Value.Offset + I}]} << (I * 8);
    if (Value.Size < 8)
      Result &= (uint64_t{1} << (8 * Value.Size)) - 1;
    return Result;
  };
  for (const auto &[Offset, Value] : Inputs)
    write(reg(Offset), Value);
  int BlockId = 0;
  for (unsigned Step = 0; Step < 10000; ++Step) {
    const auto &Block = Function.Blocks.at(BlockId);
    int Next = -1;
    for (const auto &Op : Block.Ops) {
      const auto A = [&] { return read(Op.Inputs[0]); };
      const auto B = [&] { return read(Op.Inputs[1]); };
      switch (Op.Opcode) {
      case NdOp::COPY:
        write(Op.Output, A());
        break;
      case NdOp::INT_ADD:
        write(Op.Output, A() + B());
        break;
      case NdOp::INT_SUB:
        write(Op.Output, A() - B());
        break;
      case NdOp::INT_XOR:
        write(Op.Output, A() ^ B());
        break;
      case NdOp::INT_LESS:
        write(Op.Output, A() < B());
        break;
      case NdOp::INT_EQUAL:
        write(Op.Output, A() == B());
        break;
      case NdOp::SELECT:
        write(Op.Output, A() ? B() : read(Op.Inputs[2]));
        break;
      case NdOp::LOAD: {
        const auto Access = lowMemoryOperands(Op);
        const uint64_t Address = read(*Access.Address);
        uint64_t Value = 0;
        for (unsigned I = 0; I < Access.AccessSize; ++I)
          Value |= uint64_t{Memory[Address + I]} << (8 * I);
        write(Op.Output, Value);
        break;
      }
      case NdOp::STORE: {
        const auto Access = lowMemoryOperands(Op);
        const uint64_t Address = read(*Access.Address);
        const uint64_t Value = read(*Access.StoredValue);
        for (unsigned I = 0; I < Access.AccessSize; ++I)
          Memory[Address + I] = static_cast<uint8_t>(Value >> (8 * I));
        break;
      }
      case NdOp::BRANCH:
      case NdOp::COND_BR: {
        const bool Taken = Op.Opcode == NdOp::BRANCH || B();
        for (int Successor : Block.Succs)
          if ((Function.Blocks[Successor].StartAddr == A()) == Taken)
            Next = Successor;
        if (Next < 0 && Block.Succs.size() == 1)
          Next = Block.Succs.front();
        break;
      }
      case NdOp::RETURN:
        return read(reg(0));
      case NdOp::NOP:
        break;
      default:
        ADD_FAILURE() << "oracle does not implement " << ndOpName(Op.Opcode);
        return std::nullopt;
      }
    }
    if (Next < 0)
      return std::nullopt;
    BlockId = Next;
  }
  return std::nullopt;
}

TEST(InterpreterSpecialization,
     ImmutableBytesSpecializeWithoutFreezingNativeInput) {
  Provider P;
  P.Image[0x300] = 7;
  P.add(0x100, {op(NdOp::COPY, reg(16), {constant(0x300)})});
  P.add(0x101, {op(NdOp::LOAD, reg(24, 1), {reg(16)})});
  P.add(0x102, {op(NdOp::INT_ADD, reg(0), {reg(8), reg(24, 1)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}}; // Deliberately unknown at entry.
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads[0].Address, 0x300u);
  for (uint64_t Input : std::initializer_list<uint64_t>{0, 1, 42, ~uint64_t{0}})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}), Input + 7);
}

TEST(InterpreterSpecialization, BusinessLoopConvergesByWeakeningConstants) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(0), {constant(0)}), jump(0x110)});
  P.add(0x110,
        {op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)}),
         op(NdOp::INT_LESS, reg(24, 1), {reg(0), reg(8)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(24, 1)})},
        0x120);
  P.add(0x120, {ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.Contexts, 3u);
  EXPECT_LE(Result.NodeEvaluations, 7u);
  EXPECT_GT(Result.NodeEvaluations, Result.Contexts);
  for (uint64_t Limit : {1ULL, 2ULL, 17ULL, 255ULL, 260ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Limit}}), Limit);
}

TEST(InterpreterSpecialization, JoinsDoNotLeakFirstPredecessorConstant) {
  Provider P;
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(8, 1)})}, 0x120);
  P.add(0x110, {op(NdOp::COPY, reg(16), {constant(7)}), jump(0x130)});
  P.add(0x120, {op(NdOp::COPY, reg(16), {constant(9)}), jump(0x130)});
  P.add(0x130, {op(NdOp::INT_ADD, reg(0), {reg(16), constant(1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 10u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 8u);
}

TEST(InterpreterSpecialization, ControlConstantsCloneSharedNativeDispatch) {
  Provider P;
  P.Image[0x300] = 0x40;
  P.Image[0x301] = 0x50;
  P.add(0x10, {op(NdOp::COND_BR, {}, {constant(0x20), reg(8, 1)})}, 0x30);
  P.add(0x20, {op(NdOp::COPY, reg(16), {constant(0x300)}), jump(0x38)});
  P.add(0x30, {op(NdOp::COPY, reg(16), {constant(0x301)}), jump(0x38)});
  P.add(0x38, {op(NdOp::LOAD, reg(24, 1), {reg(16)}),
               op(NdOp::INDIR_BR, {}, {reg(24, 1)})});
  P.add(0x40, {op(NdOp::COPY, reg(0), {constant(11)}), ret()});
  P.add(0x50, {op(NdOp::COPY, reg(0), {constant(22)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}};
  auto Result = specializeInterpreter(P, {0x10}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 11u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 22u);
  std::set<va_t> Labels;
  for (const auto &Block : Result.Residual.Blocks)
    EXPECT_TRUE(Labels.insert(Block.StartAddr).second);
  EXPECT_EQ(Result.Residual.Entry, 0x10u);
  EXPECT_EQ(Result.Residual.Blocks.front().StartAddr, 0x10u);
}

TEST(InterpreterSpecialization,
     FiniteSelectUsesLiveTargetAndPreservesBothArms) {
  Provider P;
  P.add(0x100, {op(NdOp::SELECT, reg(16),
                   {reg(8, 1), constant(0x110), constant(0x120)})});
  P.add(0x101, {op(NdOp::COPY, reg(8, 1), {constant(0, 1)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x110, {op(NdOp::COPY, reg(0), {constant(3)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(5)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::INDIR_BR), 0u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 5u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 3u);
}

TEST(InterpreterSpecialization, OrdinaryMemoryReadIsRetained) {
  Provider P;
  P.add(0x100, {op(NdOp::LOAD, reg(0, 1), {reg(8)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_EQ(execute(Result.Residual, {{8, 0x300}}, {{0x300, 42}}), 42u);
}

TEST(InterpreterSpecialization, FiniteTargetBudgetAndUnknownArmFailClosed) {
  Provider P;
  P.add(0x100, {op(NdOp::SELECT, reg(16),
                   {reg(8, 1), constant(0x110), constant(0x120)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x110, {ret()});
  P.add(0x120, {ret()});
  SpecializationOptions Options;
  Options.MaxIndirectTargets = 1;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  P.Code[0x100].Ops[0].Inputs[2] = reg(24);
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, ImmutableReadHonorsConfiguredByteOrder) {
  Provider P;
  P.Image[0x300] = 0x12;
  P.Image[0x301] = 0xAB;
  P.add(0x100, {op(NdOp::LOAD, reg(0, 2), {constant(0x300)}), ret()});
  SpecializationOptions Options;
  Options.ByteOrder = llvm::endianness::big;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  bool Found = false;
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY) {
        ASSERT_EQ(Op.NumInputs, 1);
        EXPECT_TRUE(Op.Inputs[0].isConst());
        EXPECT_EQ(Op.Inputs[0].Offset, 0x12ABu);
        Found = true;
      }
  EXPECT_TRUE(Found);
}

TEST(InterpreterSpecialization, AliasingStoreForgetsEarlierMemoryConstant) {
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {constant(0x300), constant(7, 1)}),
                op(NdOp::STORE, {}, {reg(8), constant(9, 1)}),
                op(NdOp::LOAD, reg(16, 1), {constant(0x300)}),
                op(NdOp::COPY, reg(0), {reg(16, 1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_EQ(count(Result.Residual, NdOp::STORE), 2u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0x300}}), 9u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0x400}}), 7u);
}

TEST(InterpreterSpecialization,
     PartialRegisterWritesDoNotInventUpperConstants) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(0, 1), {constant(0xA5, 1)}),
                op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{0, 0x123400}}), 0x1234A6u);
  EXPECT_EQ(execute(Result.Residual, {{0, 0xFF00}}), 0xFFA6u);
}

TEST(InterpreterSpecialization, UnknownIndirectAndOpaqueEffectsPublishNothing) {
  for (const auto &Unsupported :
       {op(NdOp::INDIR_BR, {}, {reg(8)}), op(NdOp::CALL, {}, {constant(0x200)}),
        op(NdOp::INTRINSIC, reg(0), {constant(0)}),
        op(NdOp::INT_DIV, reg(0), {reg(0), reg(8)})}) {
    Provider P;
    P.add(0x100, {Unsupported});
    auto Result = specializeInterpreter(P, {0x100});
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_FALSE(Result.Diagnostic.empty());
  }
}

TEST(InterpreterSpecialization, AtomicAndSegmentedEffectsAreRefused) {
  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    Provider P;
    LowOp Load = op(NdOp::LOAD, reg(0), {reg(8)});
    if (Variant)
      Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    else
      Load.MemoryOrdering = NdMemoryOrdering::Acquire;
    P.add(0x100, {Load, ret()});
    auto Result = specializeInterpreter(P, {0x100});
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     ContextAndOperationBudgetsNeverPublishPartialCFG) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(16), {constant(0)}), jump(0x110)});
  P.add(0x110,
        {op(NdOp::INT_ADD, reg(16), {reg(16), constant(1)}), jump(0x110)});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}};
  Options.MaxContextsPerAddress = 3;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  Options.ControlRegisters.clear();
  Options.MaxOperations = 2;
  Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, MalformedProviderCertificatesAreRefused) {
  Provider P;
  P.Image[0x300] = 7;
  P.MalformedRead = true;
  P.add(0x100, {op(NdOp::LOAD, reg(0, 1), {constant(0x300)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  P.MalformedRead = false;
  P.Code[0x100].Origin.OpCount = 1;
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, ConstantSnapshotExcludesMemoryAndUnknownBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, 0, Ctx.mkConst(16, 0xABCD));
  State.write(SymSpace::Register, 1, Ctx.mkVar("dynamic", 8));
  State.write(SymSpace::Temporary, 16, Ctx.mkConst(8, 7));
  State.store(Ctx.mkConst(64, 0x300), Ctx.mkConst(8, 42));
  const auto Before = State.numLiveBytes();
  const auto Snapshot = State.constantScalarBytes();
  ASSERT_EQ(Snapshot.size(), 2u);
  EXPECT_EQ(Snapshot[0].Space, SymSpace::Register);
  EXPECT_EQ(Snapshot[0].Offset, 0u);
  EXPECT_EQ(Snapshot[0].Value, 0xCDu);
  EXPECT_EQ(Snapshot[1].Space, SymSpace::Temporary);
  EXPECT_EQ(Snapshot[1].Value, 7u);
  EXPECT_EQ(State.numLiveBytes(), Before);
}

TEST(InterpreterSpecialization,
     SpilledCursorAndBusinessLoopUseFiniteEntryFrameFacts) {
  Provider P;
  P.Image[0x300] = 0x40;
  P.Image[0x301] = 0x50;
  P.add(0x10, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(16)}),
               op(NdOp::COPY, reg(40), {reg(32)}),
               op(NdOp::INT_ADD, reg(48), {reg(40), constant(8)}),
               op(NdOp::STORE, {}, {reg(40), constant(0)}),
               op(NdOp::STORE, {}, {reg(48), constant(0x300)}), jump(0x30)});
  P.add(0x30, {op(NdOp::LOAD, reg(16), {reg(48)}),
               op(NdOp::LOAD, reg(24, 1), {reg(16)}),
               op(NdOp::INDIR_BR, {}, {reg(24, 1)})});
  P.add(0x40, {op(NdOp::LOAD, reg(0), {reg(40)}),
               op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)}),
               op(NdOp::STORE, {}, {reg(40), reg(0)}),
               op(NdOp::INT_ADD, reg(16), {reg(16), constant(1)}),
               op(NdOp::STORE, {}, {reg(48), reg(16)}), jump(0x30)});
  P.add(0x50,
        {op(NdOp::LOAD, reg(0), {reg(40)}),
         op(NdOp::INT_LESS, reg(56, 1), {reg(0), reg(8)}),
         op(NdOp::COND_BR, {}, {constant(0x51), reg(56, 1)})},
        0x60);
  P.add(0x51, {op(NdOp::STORE, {}, {reg(48), constant(0x300)}), jump(0x30)});
  P.add(0x60, {ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlFrameSlots = {{-8, 8}};
  auto Result = specializeInterpreter(P, {0x10}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LE(Result.Contexts, 10u);
  EXPECT_LE(Result.NodeEvaluations, 40u);
  EXPECT_GT(count(Result.Residual, NdOp::LOAD), 0u);
  EXPECT_GT(count(Result.Residual, NdOp::STORE), 0u);
  for (uint64_t Limit : {1ULL, 2ULL, 17ULL, 260ULL})
    for (uint64_t Stack : {0x1000ULL, 0x9000ULL})
      EXPECT_EQ(execute(Result.Residual, {{8, Limit}, {32, Stack}}), Limit);
}

TEST(InterpreterSpecialization, MayAliasStoreInvalidatesFrameControlFacts) {
  for (bool Absolute : {false, true}) {
    Provider P;
    P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                  op(NdOp::STORE, {}, {reg(40), constant(0x120)}),
                  op(NdOp::STORE, {},
                     {Absolute ? constant(0x300) : reg(8), constant(0x130)}),
                  jump(0x110)});
    P.add(0x110, {op(NdOp::LOAD, reg(16), {reg(40)}),
                  op(NdOp::INDIR_BR, {}, {reg(16)})});
    P.add(0x120, {ret()});
    P.add(0x130, {ret()});
    SpecializationOptions Options;
    Options.FrameBaseRegister = SymRegisterRange{32, 8};
    Options.ControlFrameSlots = {{-8, 8}};
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
        << Result.Diagnostic;
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     FrameJoinLosesConflictsUnlessSlotIsAContextHint) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(8, 1)})},
        0x120);
  P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(0x140)}), jump(0x130)});
  P.add(0x120, {op(NdOp::STORE, {}, {reg(40), constant(0x150)}), jump(0x130)});
  P.add(0x130, {op(NdOp::LOAD, reg(16), {reg(40)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  P.add(0x150, {op(NdOp::COPY, reg(0), {constant(9)}), ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  auto Joined = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Joined.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Joined.Residual.Blocks.empty());
  Options.ControlFrameSlots = {{-8, 8}};
  auto Partitioned = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Partitioned.complete()) << Partitioned.Diagnostic;
  EXPECT_EQ(execute(Partitioned.Residual, {{8, 1}, {32, 0x1000}}), 7u);
  EXPECT_EQ(execute(Partitioned.Residual, {{8, 0}, {32, 0x1000}}), 9u);
}

TEST(InterpreterSpecialization, ChangedFrameRegisterIsNotReboundToEntryRoot) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                op(NdOp::STORE, {}, {reg(40), constant(0x120)}),
                op(NdOp::COPY, reg(32), {reg(8)}), jump(0x110)});
  P.add(0x110, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                op(NdOp::LOAD, reg(16), {reg(40)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x120, {ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlFrameSlots = {{-8, 8}};
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     RegionSnapshotObservesUnifiedAliasInvalidation) {
  SymContext Ctx;
  SymState State(Ctx);
  const SymRef Root = Ctx.mkFreshVar(64, "entry_frame");
  EXPECT_TRUE(State.constantRegionBytes(Root).empty());
  EXPECT_EQ(State.numMemoryRegions(), 0u);
  State.store(Ctx.mkSub(Root, Ctx.mkConst(64, 8)), Ctx.mkConst(16, 0xABCD));
  const auto Snapshot = State.constantRegionBytes(Root);
  ASSERT_EQ(Snapshot.size(), 2u);
  EXPECT_EQ(Snapshot[0].Offset, uint64_t(-8));
  EXPECT_EQ(Snapshot[0].Value, 0xCDu);
  State.store(Ctx.mkFreshVar(64, "may_alias"), Ctx.mkConst(8, 0));
  EXPECT_TRUE(State.constantRegionBytes(Root).empty());
}

SpecializationOptions ordinaryReturnOptions() {
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.RequireRestoredFrameAtReturn = true;
  return Options;
}

TEST(InterpreterSpecialization, PushTargetRetIsNotARecoveredFunctionReturn) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(8)}),
                op(NdOp::STORE, {}, {reg(32), constant(0x200)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, ordinaryReturnOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("restored entry frame"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, ReturnSlotOverwriteIsRefusedImmediately) {
  for (bool StraddlesEntry : {false, true}) {
    Provider P;
    if (StraddlesEntry)
      P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(1)}),
                    op(NdOp::STORE, {}, {reg(40), constant(0, 2)}), ret()});
    else
      P.add(0x100, {op(NdOp::STORE, {}, {reg(32), constant(0x200)}), ret()});
    auto Options = ordinaryReturnOptions();
    Options.ExternalStoresPreserveEntryReturnSlot = true;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(Result.Diagnostic.find("entry return control slot"),
              std::string::npos);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     NonAffineFrameAddressSurvivesProjectionAsUnsafe) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_XOR, reg(40), {reg(32), reg(8)}), jump(0x110)});
  P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(0x200)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, NonAffineFramePointerSpillReloadRemainsUnsafe) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_XOR, reg(40), {reg(32), reg(8)}),
                op(NdOp::INT_ADD, reg(48), {reg(32), constant(48)}),
                op(NdOp::STORE, {}, {reg(48), reg(40)}), jump(0x110)});
  P.add(0x110, {op(NdOp::LOAD, reg(56), {reg(48)}), jump(0x120)});
  P.add(0x120, {op(NdOp::STORE, {}, {reg(56), constant(0x200)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     ExternalStoreRequiresExplicitSourceABIContract) {
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {reg(8), constant(42)}),
                op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  auto Options = ordinaryReturnOptions();
  auto Refused = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Refused.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Refused.Diagnostic.find("nonalias contract"), std::string::npos);
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Accepted = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Accepted.complete()) << Accepted.Diagnostic;
  EXPECT_EQ(count(Accepted.Residual, NdOp::STORE), 1u);
  EXPECT_EQ(execute(Accepted.Residual, {{8, 0x300}, {32, 0x1000}}), 7u);
}

TEST(InterpreterSpecialization, ExternalPointerFrameSpillPreservesProvenance) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(16)}),
                op(NdOp::STORE, {}, {reg(32), reg(8)}), jump(0x110)});
  P.add(0x110, {op(NdOp::LOAD, reg(40), {reg(32)}),
                op(NdOp::STORE, {}, {reg(40), constant(42)}), jump(0x120)});
  P.add(0x120, {op(NdOp::LOAD, reg(40), {reg(32)}),
                op(NdOp::STORE, {}, {reg(40), constant(43)}),
                op(NdOp::INT_ADD, reg(32), {reg(32), constant(16)}),
                op(NdOp::COPY, reg(0), {constant(9)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 2u);
  EXPECT_EQ(count(Result.Residual, NdOp::STORE), 3u);
  for (uint64_t Out : {0x300ULL, 0x2000ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Out}, {32, 0x1000}}), 9u);
}

TEST(InterpreterSpecialization, UndefinedTemporaryIsNotAnExternalABIInput) {
  Provider P;
  P.add(0x100,
        {op(NdOp::STORE, {}, {NdVar::tmp(0x10000, 8), constant(0)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  P.add(0x100,
        {op(NdOp::COPY, NdVar::tmp(0x10000, 8), {reg(8)}),
         op(NdOp::STORE, {}, {NdVar::tmp(0x10000, 8), constant(0)}), ret()});
  Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_TRUE(Result.complete()) << Result.Diagnostic;
}

} // namespace

//===- BinaryUndefinedIndependenceTests.cpp - Lifted arbitrary bits
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/LowIRUndefinedIndependence.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRIndependenceStatus;

struct BinaryProgram {
  Decoder Decode;
  LowFunc Function;
  std::vector<LowIRUndefinedInstruction> Records;
  LowIRIndependenceContract Contract;

  BinaryProgram() {
    EXPECT_TRUE(Decode.init(Arch::X64));
    Decode.setStrict(true);
    Function.Entry = 0x100;
    Contract.ReturnRegisters = {{x86reg::RAX, 8}};
  }

  void block(va_t Address, std::initializer_list<uint8_t> Bytes,
             std::vector<int> Successors = {}) {
    LowBlock Block;
    Block.Id = static_cast<int>(Function.Blocks.size());
    Block.StartAddr = Address;
    Block.EndAddr = Address + Bytes.size();
    Block.Succs = std::move(Successors);
    size_t Offset = 0;
    while (Offset < Bytes.size()) {
      DecodedInsn Insn{};
      const int Size =
          Decode.decodeOneForLift(Bytes.begin() + Offset, Bytes.size() - Offset,
                                  Address + Offset, Insn);
      ASSERT_GT(Size, 0);
      LowIRUndefinedInstruction Record;
      Record.BlockId = Block.Id;
      auto &Boundary = Record.Boundary;
      Boundary.Address = Insn.Addr;
      Boundary.Size = Insn.Size;
      Boundary.FirstOp = Block.Ops.size();
      ASSERT_NO_THROW(
          Decode.liftToLow(Insn, Block.Ops, {}, {}, &Record.Effects));
      Boundary.OpCount = Block.Ops.size() - Boundary.FirstOp;
      for (size_t I = Boundary.FirstOp; I < Block.Ops.size(); ++I) {
        const auto &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::RETURN) {
          Boundary.Control = LowInstructionControl::Return;
          Boundary.ControlFlags = LowInstructionControlFlag::Return;
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
          Boundary.Control = LowInstructionControl::Branch;
          Boundary.ControlFlags = LowInstructionControlFlag::Branch;
          if (Op.Opcode == NdOp::COND_BR)
            Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
          ASSERT_TRUE(Op.Inputs[0].isConst());
          Boundary.Immediate = Op.Inputs[0].Offset;
        }
      }
      Block.InstructionBoundaries.push_back(Boundary);
      Records.push_back(std::move(Record));
      Offset += Size;
    }
    Function.Blocks.push_back(std::move(Block));
  }

  void expect(Status Expected) {
    const auto Result =
        checkLowIRUndefinedIndependence(Function, Records, Contract);
    EXPECT_EQ(Result.Status, Expected) << Result.Diagnostic;
    EXPECT_EQ(Result.Certificate.has_value(), Expected == Status::Proved);
  }
};

TEST(BinaryUndefinedIndependence,
     LogicResultIsIndependentButAuxiliaryCarryIsNot) {
  BinaryProgram Program;
  Program.block(0x100, {0x31, 0xc0, 0xc3}); // xor eax,eax; ret
  Program.expect(Status::Proved);
  Program.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
  Program.expect(Status::Dependent);
}

TEST(BinaryUndefinedIndependence, FlagSnapshotsShareOnlyTheirActualProducer) {
  BinaryProgram Shared;
  // xor ecx,ecx; lahf; mov dl,ah; lahf; xor dl,ah; movzx eax,dl; ret.
  Shared.block(0x100, {0x31, 0xc9, 0x9f, 0x88, 0xe2, 0x9f, 0x30, 0xe2, 0x0f,
                       0xb6, 0xc2, 0xc3});
  Shared.expect(Status::Proved);

  BinaryProgram Fresh;
  // An intervening XOR produces a new AF before the second LAHF snapshot.
  Fresh.block(0x100, {0x31, 0xc9, 0x9f, 0x88, 0xe2, 0x31, 0xc9, 0x9f, 0x30,
                      0xe2, 0x0f, 0xb6, 0xc2, 0xc3});
  Fresh.expect(Status::Dependent);
}

TEST(BinaryUndefinedIndependence, SnapshotSpillSurvivesUntilOverwritten) {
  BinaryProgram Spill;
  // xor eax,eax; lahf; mov [rsp-8],ah; mov cl,[rsp-8]; xor cl,ah;
  // movzx eax,cl; ret. The return is zero, but the spill still exposes AF.
  Spill.block(0x100, {0x31, 0xc0, 0x9f, 0x88, 0x64, 0x24, 0xf8, 0x8a, 0x4c,
                      0x24, 0xf8, 0x30, 0xe1, 0x0f, 0xb6, 0xc1, 0xc3});
  Spill.Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -16, 8};
  Spill.expect(Status::Dependent);
  Spill.Contract.ObserveWrittenFrameBytes = false;
  Spill.expect(Status::Proved);

  BinaryProgram Killed;
  // Same program, with mov byte ptr [rsp-8],0 before returning.
  Killed.block(0x100, {0x31, 0xc0, 0x9f, 0x88, 0x64, 0x24, 0xf8, 0x8a,
                       0x4c, 0x24, 0xf8, 0x30, 0xe1, 0x0f, 0xb6, 0xc1,
                       0xc6, 0x44, 0x24, 0xf8, 0x00, 0xc3});
  Killed.Contract.Frame = Spill.Contract.Frame;
  Killed.expect(Status::Proved);
}

TEST(BinaryUndefinedIndependence,
     OriginalBranchMustNotAssumeItsUndefinedGuard) {
  BinaryProgram Program;
  // xor eax,eax; lahf; test ah,0x10; jnz 0x110.
  Program.block(0x100, {0x31, 0xc0, 0x9f, 0xf6, 0xc4, 0x10, 0x75, 0x08},
                {1, 2});
  Program.block(0x108, {0xb8, 0, 0, 0, 0, 0xc3});
  Program.block(0x110, {0xb8, 0, 0, 0, 0, 0xc3});
  Program.expect(Status::Dependent);

  BinaryProgram Defined;
  // test edi,edi; jnz 0x110. Ordinary entry inputs remain shared.
  Defined.block(0x100, {0x85, 0xff, 0x75, 0x0c}, {1, 2});
  Defined.block(0x104, {0xb8, 0, 0, 0, 0, 0xc3});
  Defined.block(0x110, {0xb8, 1, 0, 0, 0, 0xc3});
  Defined.expect(Status::Proved);
}

TEST(BinaryUndefinedIndependence,
     UncoveredNativeInstructionRefusesCertificate) {
  BinaryProgram Program;
  Program.block(0x100, {0xd1, 0xc0, 0xc3}); // rol eax,1; ret
  Program.expect(Status::Unsupported);
}
TEST(BinaryUndefinedIndependence, ShiftGuardsBindTheCompleteOperationDigest) {
  BinaryProgram P;
  P.block(0x100, {0xd2, 0xe0, 0xc3}); // shl al,cl; ret.
  P.expect(Status::Proved);
  const auto &Effect = P.Records.front().Effects.Effects.front();
  ASSERT_TRUE(Effect.When.has_value());
  const auto After = Effect.AfterOp;
  ASSERT_GT(After, 0u);
  auto &Guard = P.Function.Blocks.front().Ops[After - 1];
  ASSERT_EQ(Guard.Opcode, NdOp::INT_LESSEQUAL);
  Guard.Inputs[0] = NdVar::scalar(31, Guard.Inputs[0].Size);
  P.expect(Status::Invalid);
}

} // namespace

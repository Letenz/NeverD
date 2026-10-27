//===- NativeStateAuditTests.cpp - String/control admission -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native bytes, complete effect receipts, and independent state oracles.
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/arch/x86_64/StringTransfer.h"
#include "../../lib/analysis/core/RecoveryArchitecture.h"
#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/symbolic/SymExec.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

constexpr uint64_t CodeAddress = 0x1000, MemoryBase = 0x8000;
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

std::vector<LowOp> lift(llvm::ArrayRef<uint8_t> Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
            int(Bytes.size()));
  if (!Insn.Raw)
    return {};
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  Dec.liftToLow(Insn, Plain);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  return Ops;
}

BinaryImage image(uint64_t Base = MemoryBase, unsigned Size = 256) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = Base;
  Memory.Size = Size;
  Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  for (unsigned I = 0; I != Size; ++I)
    Memory.Data.push_back(uint8_t(19 + I * 29 + I / 7));
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

struct StateOracle {
  std::array<uint64_t, 16> Registers;
  std::array<std::vector<uint8_t>, x86reg::VectorRegCount> Vectors;
  std::array<uint8_t, Flags.size()> FlagValues;
  uint32_t MXCSR = 0x1f80;

  explicit StateOracle(NdOpEmulator &Emulator, unsigned Seed = 0) {
    for (unsigned I = 0; I != Registers.size(); ++I)
      setRegister(Emulator, I * 8, UINT64_C(0xabcdee0012345000) + I * 61);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      for (unsigned B = 0; B != 64; ++B)
        Vectors[I].push_back(uint8_t(13 + Seed * 71 + I * 53 + B * 29));
      Emulator.setRegisterBytes(x86reg::vectorReg(I), Vectors[I]);
    }
    for (unsigned I = 0; I != Flags.size(); ++I) {
      FlagValues[I] = (Seed + I) % 2;
      Emulator.setRegister(Flags[I], FlagValues[I]);
    }
    Emulator.setMXCSR(MXCSR);
  }

  void setRegister(NdOpEmulator &Emulator, unsigned Offset, uint64_t Value) {
    Registers[Offset / 8] = Value;
    Emulator.setRegister(Offset, Value);
  }

  void setDirection(NdOpEmulator &Emulator, bool Backward) {
    FlagValues[5] = Backward;
    Emulator.setRegister(x86reg::DF, Backward);
  }

  void expect(const NdOpEmulator &Emulator) const {
    for (unsigned I = 0; I != Registers.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(I * 8),
                std::optional<uint64_t>(Registers[I]))
          << "GPR " << I;
    for (unsigned I = 0; I != Vectors.size(); ++I)
      EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(I)),
                std::optional<std::vector<uint8_t>>(Vectors[I]))
          << "complete vector " << I;
    for (unsigned I = 0; I != Flags.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(Flags[I]),
                std::optional<uint64_t>(FlagValues[I]));
    EXPECT_EQ(Emulator.getMXCSR(), MXCSR);
  }
};

void expectMemory(NdOpEmulator &Emulator, uint64_t Base,
                  const std::vector<uint8_t> &Expected) {
  for (unsigned I = 0; I != Expected.size(); ++I) {
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(0x71000000, 1);
    Load.addInput(NdVar::cst(Base + I, 8));
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(Load.Output.Offset),
              std::optional<uint64_t>(Expected[I]))
        << "byte " << I;
  }
}

TEST(X86NativeStateAudit, RepMovsqRetainsOrderedCopyAndCompleteState) {
  const auto Ops = lift({0xf3, 0x48, 0xa5});
  ASSERT_FALSE(Ops.empty());
  const auto It = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return detail::stringTransferShape(Op).has_value();
  });
  ASSERT_NE(It, Ops.end());
  EXPECT_EQ(It->Inputs[1], NdVar::reg(x86reg::RSI, 8));
  EXPECT_EQ(It->Inputs[2], NdVar::reg(x86reg::RDI, 8));
  EXPECT_EQ(It->Inputs[3], NdVar::reg(x86reg::RCX, 8));
  EXPECT_EQ(It->Inputs[4].Size, X86RepStatusBytes);
  EXPECT_EQ(detail::stringTransferShape(*It)->CompleteState, true);
  for (bool Backward : {false, true})
    for (unsigned Count : {0, 1, 3, 7})
      for (int Gap : {-32, -8, -1, 0, 1, 8, 32})
        for (unsigned Seed : {0, 1}) {
          SCOPED_TRACE(::testing::Message() << Backward << '/' << Count << '/'
                                            << Gap << '/' << Seed);
          BinaryImage Image = image();
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator, Seed);
          const int Source = Backward ? 160 : 64;
          const int Destination = Source + Gap;
          Expected.setRegister(Emulator, x86reg::RSI, MemoryBase + Source);
          Expected.setRegister(Emulator, x86reg::RDI, MemoryBase + Destination);
          Expected.setRegister(Emulator, x86reg::RCX, Count);
          Expected.setDirection(Emulator, Backward);
          auto Memory = Image.Segments[0].Data;
          // Per-element snapshot deliberately differs from memcpy/memmove:
          // overlap can feed a completed element into the next source read.
          for (unsigned I = 0; I != Count; ++I) {
            const int Delta = (Backward ? -1 : 1) * int(I * 8);
            std::array<uint8_t, 8> Element;
            std::copy_n(Memory.begin() + Source + Delta, 8, Element.begin());
            std::copy(Element.begin(), Element.end(),
                      Memory.begin() + Destination + Delta);
          }
          auto Expanded = detail::lowerStringTransfer(
              *It, Count, Backward, Ops, 0x70000000, Count * 4 + 3);
          ASSERT_TRUE(bool(Expanded)) << llvm::toString(Expanded.takeError());
          Expanded->insert(Expanded->begin(), Ops.begin(), It);
          Expanded->insert(Expanded->end(), It + 1, Ops.end());
          const uint64_t Delta = Backward ? uint64_t(0) - Count * 8 : Count * 8;
          Expected.Registers[x86reg::RSI / 8] += Delta;
          Expected.Registers[x86reg::RDI / 8] += Delta;
          Expected.Registers[x86reg::RCX / 8] = 0;
          EXPECT_EQ(Emulator.run(*Expanded), Expanded->size());
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Memory);
        }
}

TEST(X86NativeStateAudit, RepMovsqZeroCountAvoidsMemoryAndBudgetIsExact) {
  const auto Ops = lift({0xf3, 0x48, 0xa5});
  ASSERT_FALSE(Ops.empty());
  const auto It = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &Op) {
    return detail::stringTransferShape(Op).has_value();
  });
  ASSERT_NE(It, Ops.end());
  auto Exact = detail::lowerStringTransfer(*It, 7, true, Ops, 0x70000000, 31);
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  for (uint64_t Count : {uint64_t(7), UINT64_MAX}) {
    auto Refused =
        detail::lowerStringTransfer(*It, Count, true, Ops, 0x70000000, 30);
    EXPECT_FALSE(bool(Refused));
    if (!Refused)
      llvm::consumeError(Refused.takeError());
  }
  for (bool Backward : {false, true}) {
    auto Zero =
        detail::lowerStringTransfer(*It, 0, Backward, Ops, 0x70000000, 3);
    ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
    Zero->insert(Zero->begin(), Ops.begin(), It);
    Zero->insert(Zero->end(), It + 1, Ops.end());
    for (const auto &Op : *Zero) {
      EXPECT_NE(Op.Opcode, NdOp::LOAD);
      EXPECT_NE(Op.Opcode, NdOp::STORE);
    }
    BinaryImage Image = image();
    Image.Segments.clear();
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    StateOracle Expected(Emulator);
    Expected.setRegister(Emulator, x86reg::RSI, UINT64_MAX);
    Expected.setRegister(Emulator, x86reg::RDI, 0);
    Expected.setRegister(Emulator, x86reg::RCX, 0);
    Expected.setDirection(Emulator, Backward);
    EXPECT_EQ(Emulator.run(*Zero), Zero->size());
    Expected.expect(Emulator);
  }
}

class TransferProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;

  void add(va_t Address, unsigned Bytes, std::vector<LowOp> Ops) {
    SpecializationInstruction Instruction;
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = Bytes;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Address + Bytes};
    for (size_t I = 0; I != Ops.size(); ++I) {
      Ops[I].Addr = Address;
      Ops[I].Seq = int(I);
      if (Ops[I].Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      }
    }
    Instruction.Ops = std::move(Ops);
    Code.emplace(Address, std::move(Instruction));
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto Found = Code.find(Cursor.Address);
    if (Found == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing state-audit test instruction");
    return Found->second;
  }

  std::optional<SpecializationImmutableRead> immutableRead(va_t,
                                                           uint16_t) override {
    return std::nullopt;
  }
};

TEST(X86NativeStateAudit, RepAdmissionStillRequiresCountAndDirectionProofs) {
  const auto Ops = lift({0xf3, 0x48, 0xa5});
  ASSERT_FALSE(Ops.empty());
  for (unsigned Case = 0; Case != 3; ++Case) {
    TransferProvider Provider;
    LowOp Entry;
    Entry.Opcode = Case ? NdOp::COPY : NdOp::NOP;
    if (Case) {
      Entry.Output = NdVar::reg(x86reg::RCX, 8);
      Entry.addInput(NdVar::scalar(Case == 1 ? 1 : 0, 8));
    }
    Provider.add(CodeAddress - 1, 1, {Entry});
    Provider.add(CodeAddress, 3, Ops);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Provider.add(CodeAddress + 3, 1, {Return});
    SpecializationOptions Options;
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    const auto Result =
        specializeInterpreter(Provider, {CodeAddress - 1}, Options);
    if (Case == 2) {
      // Zero count needs neither a direction proof nor mapped source bytes.
      EXPECT_TRUE(Result.complete()) << Result.Diagnostic;
    } else {
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
          << Result.Diagnostic;
      EXPECT_NE(Result.Diagnostic.find("proved count and direction"),
                std::string::npos);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
    }
  }
}

struct LoadCase {
  std::vector<uint8_t> Bytes;
  std::vector<std::pair<unsigned, uint64_t>> Registers;
  uint64_t Address;
  uint64_t Base = MemoryBase;
};

TEST(X86NativeStateAudit, LdmxcsrReadsFourExactBytesAndPreservesOtherState) {
  const LoadCase Cases[] = {
      {{0x0f, 0xae, 0x10}, {{x86reg::RAX, MemoryBase + 1}}, MemoryBase + 1},
      {{0x0f, 0xae, 0x54, 0x4b, 0xfd},
       {{x86reg::RBX, MemoryBase + 1}, {x86reg::RCX, 2}},
       MemoryBase + 2},
      {{0x41, 0x0f, 0xae, 0x10},
       {{x86reg::R8, MemoryBase + 3}},
       MemoryBase + 3},
      {{0x42, 0x0f, 0xae, 0x14, 0},
       {{x86reg::RAX, MemoryBase}, {x86reg::R8, 4}},
       MemoryBase + 4},
      {{0x0f, 0xae, 0x15, 0xfe, 0x6f, 0, 0}, {}, MemoryBase + 5},
      {{0x67, 0x0f, 0xae, 0x15, 0xfe, 0x6f, 0, 0}, {}, MemoryBase + 6},
      {{0x67, 0x0f, 0xae, 0x14, 0x25, 7, 0x80, 0, 0}, {}, MemoryBase + 7},
      {{0x67, 0x0f, 0xae, 0x50, 0x40},
       {{x86reg::RAX, UINT64_C(0xcafebabefffffff0)}},
       0x30,
       0x20}};
  for (const auto &C : Cases) {
    const auto Ops = lift(C.Bytes);
    ASSERT_FALSE(Ops.empty());
    ASSERT_EQ(Ops.back().Opcode, NdOp::INTRINSIC);
    EXPECT_EQ(Ops.back().Inputs[0].Offset, uint64_t(Intrinsic::Ldmxcsr));
    EXPECT_EQ(Ops.back().MemoryAddressSpace, NdMemoryAddressSpace::Default);
    for (uint32_t Word :
         {0U, 0x1f80U, 0x5f80U, 0xffbfU, 0x10000U, UINT32_MAX}) {
      BinaryImage Image = image(C.Base);
      for (unsigned I = 0; I != 4; ++I)
        Image.Segments[0].Data[C.Address - C.Base + I] =
            uint8_t(Word >> (8 * I));
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      StateOracle Expected(Emulator);
      for (const auto &[Offset, Value] : C.Registers)
        Expected.setRegister(Emulator, Offset, Value);
      const bool Reserved = (Word & 0xffff0000U) != 0;
      if (!Reserved)
        Expected.MXCSR = Word;
      EXPECT_EQ(Emulator.run(Ops), Ops.size() - unsigned(Reserved));
      Expected.expect(Emulator);
      expectMemory(Emulator, C.Base, Image.Segments[0].Data);
    }
  }
}

TEST(X86NativeStateAudit, LdmxcsrMissingBytesDoNotCommitState) {
  const auto Ops = lift({0x0f, 0xae, 0x10});
  ASSERT_FALSE(Ops.empty());
  for (unsigned Available = 0; Available != 4; ++Available) {
    BinaryImage Image = image(MemoryBase, Available);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    StateOracle Expected(Emulator);
    Expected.setRegister(Emulator, x86reg::RAX, MemoryBase);
    EXPECT_EQ(Emulator.run(Ops), Ops.size() - 1);
    Expected.expect(Emulator);
    expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
  }
}

TEST(X86NativeStateAudit,
     AdmissionDoesNotSupplyMXCSRContextOrReservedBitProof) {
  const auto Ops = lift({0x0f, 0xae, 0x10});
  ASSERT_FALSE(Ops.empty());
  const auto &Load = Ops.back();
  EXPECT_FALSE(supportsRuntimeControlOperation(Load, {}));
  for (uint32_t Mask : {0xffbfU, 0xffffU})
    for (uint32_t Value : {0x1f80U, 0x40U, 0xffffU, 0x10000U}) {
      SpecializationOptions Options;
      Options.ExplicitMachineState = true;
      Options.NormalNonfaultingExecution = true;
      Options.X64MXCSRMask = Mask;
      EXPECT_TRUE(supportsRuntimeControlOperation(Load, Options));
      EXPECT_EQ(llvm::toString(validateRuntimeControlOptions(Options)), "");
      SymContext Ctx;
      SymState State(Ctx);
      State.write(SymSpace::Register, x86reg::RAX, Ctx.mkConst(64, MemoryBase));
      State.store(Ctx.mkConst(64, MemoryBase), Ctx.mkConst(32, Value));
      SymExec Exec(Ctx, State);
      for (size_t I = 0; I + 1 < Ops.size(); ++I)
        ASSERT_EQ(Exec.step(Ops[I]), StepResult::Continue);
      RuntimeControlState Controls{{Ctx.mkConst(32, 0x1f80)}};
      uint64_t Next = 0x70000000;
      auto Transition = lowerRuntimeControlOperation(
          Load, State, Controls, Options, [&](uint16_t Bytes) {
            const auto Temporary = NdVar::tmp(Next, Bytes);
            Next += Bytes;
            return Temporary;
          });
      ASSERT_TRUE(Transition);
      ASSERT_TRUE(Transition->Rejected);
      ASSERT_TRUE(Transition->OutputState);
      EXPECT_EQ(Exec.run(Transition->Ops), Transition->Ops.size());
      const auto Bad = Ctx.asConst(Exec.operandValue(*Transition->Rejected));
      ASSERT_TRUE(Bad);
      EXPECT_EQ(Bad->getZExtValue(), uint64_t((Value & ~Mask) != 0));
      const auto Final =
          Ctx.asConst(Exec.operandValue(Transition->OutputState->second));
      ASSERT_TRUE(Final);
      EXPECT_EQ(Final->getZExtValue(), (Value & ~Mask) ? 0x1f80U : Value);
    }
}

void expectUnaudited(Decoder &Dec, DecodedInsn &Insn) {
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OperationDigest = "stale";
  Effects.Effects.push_back({1, NdVar::reg(x86reg::AF, 1), 0, 1, {}});
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  } catch (const UnliftedInstruction &) {
    // Semantic rejection also leaves undefined-output admission closed.
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
}

TEST(X86NativeStateAudit, OtherPrefixesModesAndInstructionFamiliesStayClosed) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x48, 0xa5},
      {0xf3, 0xa5},
      {0xf3, 0xa4},
      {0xf3, 0x48, 0xab},
      {0xf2, 0x48, 0xa5},
      {0xf3, 0xf3, 0x48, 0xa5},
      {0x67, 0xf3, 0x48, 0xa5},
      {0x64, 0xf3, 0x48, 0xa5},
      {0x65, 0xf3, 0x48, 0xa5},
      {0xf3, 0x49, 0xa5},
      {0x0f, 0xae, 0x18},
      {0xc5, 0xf8, 0xae, 0x10},
      {0x66, 0x0f, 0xae, 0x10},
      {0x67, 0x67, 0x0f, 0xae, 0x10},
      {0x40, 0x40, 0x0f, 0xae, 0x10},
      {0x44, 0x0f, 0xae, 0x10},
      {0x48, 0x0f, 0xae, 0x10},
      {0x64, 0x0f, 0xae, 0x10},
      {0x65, 0x0f, 0xae, 0x10}};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Bytes : Cases) {
    DecodedInsn Insn{};
    const int Size =
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn);
    if (Size <= 0)
      continue;
    ASSERT_EQ(Size, int(Bytes.size()));
    SCOPED_TRACE(Insn.Raw->mnemonic);
    expectUnaudited(Dec, Insn);
  }
  Decoder Narrow;
  ASSERT_TRUE(Narrow.init(Arch::X86));
  const uint8_t Bytes[] = {0x0f, 0xae, 0x10};
  DecodedInsn Insn{};
  ASSERT_EQ(Narrow.decodeOneForLift(Bytes, sizeof(Bytes), CodeAddress, Insn),
            3);
  expectUnaudited(Narrow, Insn);
}

TEST(X86NativeStateAudit, RawAndDetailDisagreementCannotAuthorizeEffects) {
  using Mutation = void (*)(cs_insn &);
  const Mutation Common[] = {
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.detail->x86.op_count += 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 1; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.rex ^= 1; },
      [](cs_insn &I) { I.detail->x86.opcode[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.prefix[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.base = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.index = X86_REG_R9; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.scale = 3; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.disp += 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].mem.segment = X86_REG_FS; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; }};
  const Mutation Rep[] = {
      [](cs_insn &I) { I.bytes[0] = 0xf2; },
      [](cs_insn &I) { I.bytes[1] = 0x40; },
      [](cs_insn &I) { I.bytes[2] = 0xa4; },
      [](cs_insn &I) { I.id = X86_INS_STOSQ; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.base = X86_REG_RDI; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.segment = X86_REG_GS; },
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset = 1; }};
  const Mutation MXCSR[] = {
      [](cs_insn &I) { I.bytes[2] ^= 8; },
      [](cs_insn &I) { I.bytes[3] ^= 8; },
      [](cs_insn &I) { I.id = X86_INS_STMXCSR; },
      [](cs_insn &I) { I.detail->x86.modrm ^= 8; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset += 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset -= 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 4; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (bool String : {false, true}) {
    const std::vector<uint8_t> Bytes =
        String ? std::vector<uint8_t>{0xf3, 0x48, 0xa5}
               : std::vector<uint8_t>{0x0f, 0xae, 0x54, 0x4b, 0xfd};
    const auto Specific = String ? llvm::ArrayRef<Mutation>(Rep)
                                 : llvm::ArrayRef<Mutation>(MXCSR);
    std::vector<Mutation> Mutations(std::begin(Common), std::end(Common));
    Mutations.insert(Mutations.end(), Specific.begin(), Specific.end());
    for (unsigned Index = 0; Index != Mutations.size(); ++Index) {
      SCOPED_TRACE(::testing::Message() << String << '/' << Index);
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
          int(Bytes.size()));
      Mutations[Index](*Insn.Raw);
      expectUnaudited(Dec, Insn);
    }
  }
}

} // namespace

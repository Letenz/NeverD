//===- X86_64_CarryArithmeticFlagTests.cpp - Carry/borrow flags -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <bit>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {
void checkCarryArithmetic(Arch Target, unsigned Width, bool Subtract,
                          unsigned Form) {
  SCOPED_TRACE(::testing::Message() << static_cast<unsigned>(Target) << '/'
                                    << Width << '/' << Subtract << '/' << Form);
  std::vector<uint8_t> Bytes;
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Width == 8)
    Bytes.push_back(0x48);
  // adc/sbb accumulator, count; [base], count; accumulator, [base].
  const uint8_t Opcode = (Subtract ? 0x18 : 0x10) + (Width != 1);
  Bytes.push_back(Opcode + (Form == 2 ? 2 : 0));
  Bytes.push_back(Form == 0 ? 0xc8 : Form == 1 ? 0x0b : 0x03);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Target));
  Decode.setStrict(true);
  DecodedInsn Instruction{};
  ASSERT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
      static_cast<int>(Bytes.size()));
  ASSERT_EQ(Instruction.Id, Subtract ? X86_INS_SBB : X86_INS_ADC);
  std::vector<LowOp> Ops;
  ASSERT_NO_THROW(Decode.liftToLow(Instruction, Ops));
  ASSERT_FALSE(Ops.empty());
  if (Form == 1) {
    LowOp Reload;
    Reload.Opcode = NdOp::LOAD;
    Reload.Output = NdVar::reg(x86reg::RDX, Width);
    Reload.addInput(NdVar::scalar(0x4000, Target == Arch::X64 ? 8 : 4));
    Ops.push_back(Reload);
  }
  const uint64_t Mask =
      Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - 1;
  const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
  std::vector<uint64_t> Values = {0, Mask, Sign, Sign - 1};
  // Exercise every low nibble while higher bits independently cross signed
  // and unsigned boundaries. The AF oracle uses nibble arithmetic, not the
  // XOR identity used by the lifter.
  for (unsigned I = 0; I < 16; ++I) {
    Values.push_back(I);
    Values.push_back((Mask & ~UINT64_C(15)) | I);
  }
  for (uint64_t A : Values)
    for (uint64_t B : Values)
      for (unsigned Carry : {0u, 1u})
        for (unsigned OldAF : {0u, 1u}) {
          SCOPED_TRACE(::testing::Message()
                       << A << '/' << B << '/' << Carry << '/' << OldAF);
          BinaryImage Image;
          Image.Arch = Target;
          Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
          if (Form) {
            Segment Data;
            Data.VA = 0x4000;
            Data.Size = Data.FileSz = Width;
            Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
            Data.Data.resize(Width);
            const uint64_t Value = Form == 1 ? A : B;
            for (unsigned Byte = 0; Byte < Width; ++Byte)
              Data.Data[Byte] = Value >> (Byte * 8);
            Image.Segments.push_back(std::move(Data));
          }
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setRegister(x86reg::RAX, A);
          Emulator.setRegister(x86reg::RCX, B);
          Emulator.setRegister(x86reg::RBX, 0x4000);
          Emulator.setRegister(x86reg::CF, Carry);
          Emulator.setRegister(x86reg::AF, OldAF);
          Emulator.setRegister(x86reg::DF, 1);
          ASSERT_EQ(Emulator.run(Ops), Ops.size());
          ASSERT_FALSE(Emulator.skips().any());
          const uint64_t Result =
              (Subtract ? A - B - Carry : A + B + Carry) & Mask;
          const bool AF = Subtract ? (A & 15) < (B & 15) + Carry
                                   : (A & 15) + (B & 15) + Carry > 15;
          const bool CF = Subtract ? A < B || (Carry && A == B)
                                   : B > Mask - A || (Carry && B == Mask - A);
          const bool OF = Subtract ? ((A ^ B) & (A ^ Result) & Sign) != 0
                                   : (~(A ^ B) & (A ^ Result) & Sign) != 0;
          const auto Actual =
              Emulator.getRegister(Form == 1 ? x86reg::RDX : x86reg::RAX);
          ASSERT_TRUE(Actual.has_value());
          ASSERT_EQ(*Actual & Mask, Result);
          ASSERT_EQ(Emulator.getRegister(x86reg::AF), AF);
          ASSERT_EQ(Emulator.getRegister(x86reg::CF), CF);
          ASSERT_EQ(Emulator.getRegister(x86reg::OF), OF);
          ASSERT_EQ(Emulator.getRegister(x86reg::SF), (Result & Sign) != 0);
          ASSERT_EQ(Emulator.getRegister(x86reg::ZF), Result == 0);
          ASSERT_EQ(Emulator.getRegister(x86reg::PF),
                    (std::popcount(static_cast<uint8_t>(Result)) & 1) == 0);
          ASSERT_EQ(Emulator.getRegister(x86reg::DF), 1u);
        }
}
} // namespace

TEST(X86CarryArithmeticFlags, RegisterAndMemoryFormsDefineAuxiliaryCarry) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (bool Subtract : {false, true})
        for (unsigned Form = 0; Form < 3; ++Form)
          checkCarryArithmetic(Target, Width, Subtract, Form);
    }
}

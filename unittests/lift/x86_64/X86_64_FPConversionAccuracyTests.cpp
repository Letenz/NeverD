//===- X86_64_FPConversionAccuracyTests.cpp - Conversion regressions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
#include <immintrin.h>

namespace {
using namespace neverd;

template <typename Scalar, typename Integer>
std::pair<uint64_t, uint32_t>
nativeIntegerConversion(uint64_t Bits, uint32_t State, bool Truncate) {
  Scalar Value;
  std::memcpy(&Value, &Bits, sizeof(Value));
  Integer Result;
  const uint32_t Saved = _mm_getcsr();
  if constexpr (sizeof(Scalar) == 4) {
    if (Truncate)
      __asm__ volatile("ldmxcsr %1\n\tcvttss2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
    else
      __asm__ volatile("ldmxcsr %1\n\tcvtss2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
  } else {
    if (Truncate)
      __asm__ volatile("ldmxcsr %1\n\tcvttsd2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
    else
      __asm__ volatile("ldmxcsr %1\n\tcvtsd2si %2,%0\n\tstmxcsr %1"
                       : "=&r"(Result), "+m"(State)
                       : "x"(Value)
                       : "memory");
  }
  _mm_setcsr(Saved);
  return {Result, State};
}
} // namespace
#endif

TEST(X86FPConversionAccuracy, DenormalIntegersRaisePrecisionWithoutDenormal) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (unsigned SourceBytes : {4U, 8U})
    for (unsigned DestinationBytes : {4U, 8U})
      for (bool Truncate : {false, true})
        for (unsigned Rounding : {0U, 1U, 2U, 3U})
          for (bool DAZ : {false, true})
            for (bool DenormalMasked : {false, true})
              for (bool Negative : {false, true}) {
                const uint64_t Bits =
                    1 | (Negative
                             ? (SourceBytes == 4 ? UINT64_C(0x80000000)
                                                 : UINT64_C(0x8000000000000000))
                             : 0);
                // Precision stays masked. An unmasked DM must not fault:
                // FP-to-integer conversions do not raise denormal operand.
                const uint32_t State = 0x1e80 | (Rounding << 13) |
                                       (DAZ ? 0x40 : 0) |
                                       (DenormalMasked ? 0x100 : 0);
                const auto Expected =
                    SourceBytes == 4
                        ? (DestinationBytes == 4
                               ? nativeIntegerConversion<float, uint32_t>(
                                     Bits, State, Truncate)
                               : nativeIntegerConversion<float, uint64_t>(
                                     Bits, State, Truncate))
                        : (DestinationBytes == 4
                               ? nativeIntegerConversion<double, uint32_t>(
                                     Bits, State, Truncate)
                               : nativeIntegerConversion<double, uint64_t>(
                                     Bits, State, Truncate));
                SCOPED_TRACE(testing::Message()
                             << SourceBytes << "->" << DestinationBytes
                             << " state=" << State << " bits=" << Bits
                             << " truncate=" << Truncate);
                ASSERT_EQ(Expected.second & 2, 0U);
                NdOpEmulator Emulator(Image);
                Emulator.setStrictMode(true);
                Emulator.setMXCSR(State);
                std::vector<uint8_t> Source(16, 0);
                std::memcpy(Source.data(), &Bits, SourceBytes);
                Emulator.setRegisterBytes(x86reg::XMM0, Source);
                LowOp Op;
                Op.Opcode = NdOp::INTRINSIC;
                Op.Output = NdVar::tmp(0, 16);
                Op.addInput(NdVar::cst(
                    static_cast<unsigned>(Intrinsic::X86FPConvert), 2));
                Op.addInput(
                    NdVar::cst(makeX86FPConvertControl(
                                   X86FPConvertKind::FloatToSignedInteger,
                                   SourceBytes == 8, DestinationBytes == 8,
                                   Truncate, false,
                                   Truncate ? X86FPRounding::TowardZero
                                            : X86FPRounding::MXCSR,
                                   1),
                               2));
                Op.addInput(NdVar::reg(x86reg::XMM0, 16));
                Op.addInput(NdVar::cst(1, 1));
                ASSERT_TRUE(Emulator.step(Op));
                const auto Result = Emulator.getRegisterBytes(0);
                ASSERT_TRUE(Result);
                uint64_t Actual = 0;
                std::memcpy(&Actual, Result->data(), DestinationBytes);
                EXPECT_EQ(Actual, Expected.first);
                EXPECT_EQ(Emulator.getMXCSR(), Expected.second);
              }
#else
  GTEST_SKIP() << "native scalar conversion oracle requires x64 GCC/Clang";
#endif
}

TEST(X86FPConversionAccuracy, FloatDenormalsRetainOperandExceptionAndDaz) {
  using namespace neverd;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  for (bool DAZ : {false, true})
    for (bool DenormalMasked : {false, true}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      const uint32_t State =
          0x1e80 | (DAZ ? 0x40 : 0) | (DenormalMasked ? 0x100 : 0);
      Emulator.setMXCSR(State);
      std::vector<uint8_t> Source(16, 0);
      Source[0] = 1;
      Emulator.setRegisterBytes(x86reg::XMM0, Source);
      LowOp Op;
      Op.Opcode = NdOp::INTRINSIC;
      Op.Output = NdVar::tmp(0, 16);
      Op.addInput(
          NdVar::cst(static_cast<unsigned>(Intrinsic::X86FPConvert), 2));
      Op.addInput(NdVar::cst(
          makeX86FPConvertControl(X86FPConvertKind::FloatToFloat, false, true,
                                  false, false, X86FPRounding::MXCSR, 1),
          2));
      Op.addInput(NdVar::reg(x86reg::XMM0, 16));
      Op.addInput(NdVar::cst(1, 1));
      EXPECT_EQ(Emulator.step(Op), DAZ || DenormalMasked);
      EXPECT_EQ(Emulator.getMXCSR(), State | (DAZ ? 0 : 2));
      if (!DAZ && !DenormalMasked)
        EXPECT_FALSE(Emulator.getRegisterBytes(0));
      else {
        const auto Result = Emulator.getRegisterBytes(0);
        ASSERT_TRUE(Result);
        uint64_t Bits = 0;
        std::memcpy(&Bits, Result->data(), 8);
        EXPECT_EQ(Bits, DAZ ? 0 : UINT64_C(0x36a0000000000000)); // exact 2^-149
      }
    }
}

//===- AArch64_NativeDecodeParityFPTests.cpp - scalar floating point parity -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AArch64_NativeDecodeParityTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::a64_parity_test;

// FMOV scalar: register (Vd,Vn), general (GPR<->FP), and the 8-bit scalar
// immediate, over ptype (S/D/H + the UNALLOCATED 10), the rmode/opcode selector
// (top-half rmode!=0 declined) and every imm8 for the immediate form.
TEST_F(A64NativeParity, FmovScalarSweep) {
  const va_t A = 0xF00000;
  // Register + general: sweep M(sf), ptype, rmode, opcode, and reg fields.
  for (uint32_t M = 0; M < 2; ++M)
    for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
      for (uint32_t Rmode = 0; Rmode < 4; ++Rmode)
        for (uint32_t Opcode = 0; Opcode < 8; ++Opcode)
          for (uint32_t Rn : {0u, 1u, 31u})
            for (uint32_t Rd : {0u, 2u, 31u}) {
              uint32_t W = (M << 31) | (0x1Eu << 24) | (Ptype << 22) |
                           (1u << 21) | (Rmode << 19) | (Opcode << 16) |
                           (Rn << 5) | Rd;
              std::string D = checkOne(O, W, A);
              EXPECT_TRUE(D.empty()) << D;
            }
  // FP data-processing 1-source (opcode(20:15)==0, bits[14:10]==0b10000) hits
  // the FMOV-register form; sweep ptype and registers.
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
    for (uint32_t Rn = 0; Rn < 32; ++Rn)
      for (uint32_t Rd : {0u, 31u}) {
        uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) | (0x10u << 10) |
                     (Rn << 5) | Rd;
        std::string D = checkOne(O, W, A);
        EXPECT_TRUE(D.empty()) << D;
      }
  // Scalar immediate: ptype x every imm8 x a couple of Rd (+ a nonzero imm5
  // field, which must be declined).
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype)
    for (uint32_t Imm8 = 0; Imm8 < 256; ++Imm8)
      for (uint32_t Imm5 : {0u, 1u})
        for (uint32_t Rd : {0u, 5u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                       (Imm8 << 13) | (0x4u << 10) | (Imm5 << 5) | Rd;
          std::string D = checkOne(O, W, A);
          EXPECT_TRUE(D.empty()) << D;
        }
}
// Scalar FP data-processing: 1-source (FABS/FNEG/FSQRT/FCVT), 2-source
// (FMUL/FDIV/FADD/FSUB/FMAX/FMIN/FMAXNM/FMINNM/FNMUL), and FCMP/FCMPE (register
// and zero forms) across ptype (S/D/H + the UNALLOCATED 10) and the opcode
// fields, so every taken/declined boundary is lifted both ways.
TEST_F(A64NativeParity, FpScalarDataProcSweep) {
  const va_t A = 0xF80000;
  for (uint32_t Ptype = 0; Ptype < 4; ++Ptype) {
    // 1-source: opcode(20:15) 0..0x0F covers FMOV/FABS/FNEG/FSQRT + the FCVT
    // targets (0001TT) and some FRINT declines.
    for (uint32_t Opcode = 0; Opcode < 0x10; ++Opcode)
      for (uint32_t Rn : {0u, 31u})
        for (uint32_t Rd : {0u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                       (Opcode << 15) | (0x10u << 10) | (Rn << 5) | Rd;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // 2-source: opcode(15:12) 0..0x0F (9..15 declined).
    for (uint32_t Opcode = 0; Opcode < 0x10; ++Opcode)
      for (uint32_t Rm : {1u, 31u})
        for (uint32_t Rn : {2u, 31u}) {
          uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) | (Rm << 16) |
                       (Opcode << 12) | (2u << 10) | (Rn << 5) | 0u;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
    // FP compare: op(15:14) x opcode2(4:0) x Rm x Rn.
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Opcode2 = 0; Opcode2 < 0x20; ++Opcode2)
        for (uint32_t Rm : {0u, 2u})
          for (uint32_t Rn : {0u, 3u}) {
            uint32_t W = (0x1Eu << 24) | (Ptype << 22) | (1u << 21) |
                         (Rm << 16) | (Op << 14) | (0x8u << 10) | (Rn << 5) |
                         Opcode2;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  }
}

TEST_F(A64NativeParity, FpZeroCompareUsesRegisterWidth) {
  for (const auto &[Ptype, Bytes] :
       {std::pair{0U, 4U}, std::pair{1U, 8U}, std::pair{3U, 2U}}) {
    for (bool Signaling : {false, true}) {
      for (unsigned Register : {0U, 31U}) {
        const uint32_t Word = 0x1e202008 | (Ptype << 22) |
                              (Register << 5) | (Signaling ? 16 : 0);
        SCOPED_TRACE(Word);
        cs_insn Insn{};
        cs_detail Detail{};
        ASSERT_TRUE(a64native::tryDecode(Word, 0x1000, Insn, Detail));
        std::vector<LowOp> CapstoneOps;
        ASSERT_TRUE(O.lift(Word, 0x1000, CapstoneOps));
        // The current decoders use XZR. Decoded clients can supply a numeric
        // zero too; all three representations have the same floating width.
        for (unsigned Representation = 0; Representation < 3;
             ++Representation) {
          auto &Right = Detail.aarch64.operands[1];
          if (Representation == 0) {
            Right.type = AARCH64_OP_REG;
            Right.reg = AARCH64_REG_XZR;
          } else if (Representation == 1) {
            Right.type = AARCH64_OP_FP;
            Detail.aarch64.operands[1].fp = 0.0;
          } else {
            Right.type = AARCH64_OP_IMM;
            Right.imm = 0;
          }
          std::vector<LowOp> Ops;
          nativeLift(Word, 0x1000, Insn, Ops);
          ASSERT_EQ(Ops.size(), CapstoneOps.size());
          EXPECT_TRUE(
              std::equal(Ops.begin(), Ops.end(), CapstoneOps.begin(), lowOpEq));
          unsigned Comparisons = 0;
          for (const auto &Op : Ops) {
            if (Op.Opcode != NdOp::FLOAT_EQUAL && Op.Opcode != NdOp::FLOAT_LESS)
              continue;
            ++Comparisons;
            ASSERT_EQ(Op.NumInputs, 2U);
            EXPECT_EQ(Op.Inputs[0].Size, Bytes);
            EXPECT_EQ(Op.Inputs[1].Size, Bytes);
            const auto &Zero =
                Op.Inputs[0].isConst() ? Op.Inputs[0] : Op.Inputs[1];
            EXPECT_TRUE(Zero.isConst());
            EXPECT_EQ(Zero.Offset, 0U);
          }
          EXPECT_EQ(Comparisons, 3U);
        }
      }
    }
  }
}

} // namespace

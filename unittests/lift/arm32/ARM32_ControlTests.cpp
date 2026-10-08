#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/ARMLifter.h"

class ARM32_Control : public NeverDLiftTest {};

static fs::path testObj() {
    return fs::path(TEST_OBJ_DIR) / "test_control_arm.o";
}

TEST_F(ARM32_Control, AllStagesPass) {
    ASSERT_TRUE(fs::exists(testObj())) << "test_control_arm.o not built";
    verifyAllStages(testObj());
}

TEST_F(ARM32_Control, NoUnlifted) {
    verifyNoUnlifted(testObj());
}

TEST_F(ARM32_Control, CmpBeqLifts) {
    verifyLowIRContains(testObj(), "test_cmp_beq_arm", "INT_SUB");
}

TEST_F(ARM32_Control, TstLifts) {
    verifyLowIRContains(testObj(), "test_tst_arm", "INT_AND");
}

TEST_F(ARM32_Control, CmnLifts) {
    verifyLowIRContains(testObj(), "test_cmn_arm", "INT_ADD");
}

TEST_F(ARM32_Control, ClzLifts) {
    verifyLowIRContains(testObj(), "test_clz_arm", "LZCOUNT");
}

TEST_F(ARM32_Control, RevLifts) {
    verifyLowIRContains(testObj(), "test_rev_arm", "INT_AND");
}

TEST_F(ARM32_Control, SxtbLifts) {
    verifyLowIRContains(testObj(), "test_sxtb_arm", "INT_SEXT");
}

TEST_F(ARM32_Control, SxthLifts) {
    verifyLowIRContains(testObj(), "test_sxth_arm", "INT_SEXT");
}

TEST_F(ARM32_Control, UxtbLifts) {
    verifyLowIRContains(testObj(), "test_uxtb_arm", "INT_AND");
}

TEST_F(ARM32_Control, AdcLifts) {
    verifyLowIRContains(testObj(), "test_adc_arm", "INT_ADD");
}

TEST_F(ARM32_Control, SbcLifts) {
    verifyLowIRContains(testObj(), "test_sbc_arm", "INT_SUB");
}

TEST_F(ARM32_Control, NoUnreachableInFunctions) {
    verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

TEST(ARM32ThumbControl, CallWhoseHalfwordsLookLikeAUserRegisterLoad) {
  // `bl` 0xf0 bytes ahead, f000 f878: its two halfwords read as one A32 word
  // carry the multiple-transfer class and bit 22, the user-register form of
  // an A32 `ldm` that returns from an exception.  T32 has no such form; the
  // call lifts as a call.
  using namespace neverd;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  const uint8_t Call[] = {0x00, 0xf0, 0x78, 0xf8};
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOne(Call, sizeof(Call), 0x40119c, Insn), 4);
  ARMLifter L(Arch::ARM, InstructionMode::Thumb);
  ASSERT_TRUE(L.isStrict());
  std::vector<LowOp> Ops;
  ASSERT_NO_THROW(L.lift(Insn.Raw, Ops));
  bool Called = false;
  for (const LowOp &Op : Ops)
    Called |= Op.Opcode == NdOp::CALL && Op.NumInputs > 0 &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Offset == 0x401290;
  EXPECT_TRUE(Called);

  // A32 `ldm sp, {pc}^` is that exception return, which strict lifting
  // refuses until it has a typed contract.
  Decoder A32;
  ASSERT_TRUE(A32.init(Arch::ARM, InstructionMode::ARM));
  const uint8_t Return[] = {0x00, 0x80, 0xdd, 0xe8};
  ASSERT_EQ(A32.decodeOne(Return, sizeof(Return), 0x2000, Insn), 4);
  ARMLifter ARMMode(Arch::ARM, InstructionMode::ARM);
  Ops.clear();
  EXPECT_THROW(ARMMode.lift(Insn.Raw, Ops), UnliftedInstruction);
}

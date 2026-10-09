#include "../x86_64/X86FPStateFixture.h"

class X86_32_FP : public X86FPStateLiftTest {};

static fs::path testObj() { return fs::path(TEST_OBJ_DIR) / "test_fp32.o"; }

TEST_F(X86_32_FP, AllStagesPass) {
  ASSERT_TRUE(fs::exists(testObj())) << "test_fp32.o not built";
  verifyAllStages(testObj());
}

TEST_F(X86_32_FP, NoUnlifted) { verifyNoUnlifted(testObj()); }

TEST_F(X86_32_FP, AddssLifts) {
  verifyScalarFPState(testObj(), "test_addss32",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_32_FP, SubssLifts) {
  verifyScalarFPState(testObj(), "test_subss32",
                      neverd::Intrinsic::X86FPSubState);
}

TEST_F(X86_32_FP, MulssLifts) {
  verifyScalarFPState(testObj(), "test_mulss32",
                      neverd::Intrinsic::X86FPMulState);
}

TEST_F(X86_32_FP, DivssLifts) {
  verifyScalarFPState(testObj(), "test_divss32",
                      neverd::Intrinsic::X86FPDivState);
}

TEST_F(X86_32_FP, Cvtss2siLifts) {
  verifyLowIRContains(testObj(), "test_cvtss2si32", "FLOAT_FLOAT2INT");
}

TEST_F(X86_32_FP, Cvtsi2ssLifts) {
  verifyLowIRContains(testObj(), "test_cvtsi2ss32", "FLOAT_INT2FLOAT");
}

TEST_F(X86_32_FP, AddsdLifts) {
  verifyScalarFPState(testObj(), "test_addsd32",
                      neverd::Intrinsic::X86FPAddState);
}

TEST_F(X86_32_FP, Cvttss2siLifts) {
  verifyLowIRContains(testObj(), "test_cvttss2si32", "FLOAT_TRUNC");
}

TEST_F(X86_32_FP, NoUnreachableInFunctions) {
  verifyLLVMIRNotContains(testObj(), "", "unreachable");
}

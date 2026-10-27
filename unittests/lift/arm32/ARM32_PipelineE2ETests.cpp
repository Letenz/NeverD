#include "NeverDLiftFixture.h"

class ARM32_PipelineE2E : public NeverDLiftTest {};

static fs::path armObj(const char *name) {
  return fs::path(TEST_OBJ_DIR) / name;
}

// ============================================================================
// Full pipeline verification for every ARM32 test object
// ============================================================================

#define FULL_PIPELINE_TEST(tag, objname)                                       \
  TEST_F(ARM32_PipelineE2E, FullPipeline_##tag) {                              \
    verifyAllModesSucceed(armObj(objname));                                    \
  }                                                                            \
  TEST_F(ARM32_PipelineE2E, NoUnreachable_##tag) {                             \
    verifyLLVMIRNoUnreachable(armObj(objname));                                \
  }

FULL_PIPELINE_TEST(Arith, "test_arith_arm.o")
FULL_PIPELINE_TEST(Mem, "test_mem_arm.o")
FULL_PIPELINE_TEST(Control, "test_control_arm.o")
FULL_PIPELINE_TEST(Logic, "test_logic_arm.o")
FULL_PIPELINE_TEST(MulDiv, "test_muldiv_arm.o")
FULL_PIPELINE_TEST(Carry, "test_carry_arm.o")
FULL_PIPELINE_TEST(Extend, "test_extend_arm.o")
FULL_PIPELINE_TEST(Shift, "test_shift_arm.o")

#undef FULL_PIPELINE_TEST

// ============================================================================
// Decompile mode
// ============================================================================

#define DECOMPILE_TEST(tag, objname)                                           \
  TEST_F(ARM32_PipelineE2E, DecompileC_##tag) {                                \
    verifyDecompileProducesOutput(armObj(objname));                            \
  }

DECOMPILE_TEST(Arith, "test_arith_arm.o")
DECOMPILE_TEST(Control, "test_control_arm.o")
DECOMPILE_TEST(Mem, "test_mem_arm.o")
DECOMPILE_TEST(Logic, "test_logic_arm.o")
DECOMPILE_TEST(MulDiv, "test_muldiv_arm.o")
DECOMPILE_TEST(Carry, "test_carry_arm.o")
DECOMPILE_TEST(Extend, "test_extend_arm.o")
DECOMPILE_TEST(Shift, "test_shift_arm.o")

#undef DECOMPILE_TEST

// ============================================================================
// LLVM IR semantic preservation
// ============================================================================

TEST_F(ARM32_PipelineE2E, Semantic_Add) {
  verifyLLVMIRContains(armObj("test_arith_arm.o"), "test_add", "add");
}

TEST_F(ARM32_PipelineE2E, Semantic_StoreExists) {
  verifyLLVMIRContains(armObj("test_mem_arm.o"), "", "store");
}

TEST_F(ARM32_PipelineE2E, Semantic_LoadExists) {
  verifyLLVMIRContains(armObj("test_mem_arm.o"), "", "load");
}

// ============================================================================
// Conditional branch
// ============================================================================

TEST_F(ARM32_PipelineE2E, CondBranch_Control) {
  verifyLLVMIRHasConditionalLogic(armObj("test_control_arm.o"));
}

// A function the lift refuses is named, with why, at the head of a whole
// unit in both routes, where it would otherwise be missing with no word.
TEST_F(ARM32_PipelineE2E, WholeUnitNamesRefusedFunctions) {
  const auto Object = fs::path(TEST_OBJ_DIR) / "test_refused_functions_arm.o";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto Result = LLVM ? decompileToC(Object) : decompileToHighC(Object);
    ASSERT_EQ(Result.exitCode, 0) << Result.err;
    std::ifstream Input(tmpFile(LLVM ? "decompiled.c" : "decompiled_high.c"));
    const std::string Source((std::istreambuf_iterator<char>(Input)),
                             std::istreambuf_iterator<char>());
    EXPECT_EQ(Source.rfind("/* neverd.not-decompiled: refused_arm at 0x", 0),
              0u)
        << Source;
    EXPECT_NE(Source.find(": rejected-incomplete; unsupported instruction at "
                          "0x"),
              std::string::npos)
        << Source;
    EXPECT_NE(Source.find("decompiled_arm("), std::string::npos) << Source;
  }
}

//===- ByteCellScalarizationTests.cpp - Overlapping local cells -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ByteCellScalarizationTest.h"

#include "neverd/analysis/LLVMMemoryAnalysis.h"

namespace neverd::cell_test {
namespace {
void refuses(const std::string &Text, llvm::StringRef Layout = "e-p:64:64") {
  llvm::LLVMContext C;
  auto M = parse(C, Text, Layout);
  ASSERT_TRUE(M);
  const auto Before = print(*M);
  auto R = ByteCellScalarizationPass::scalarize(*M->getFunction("f"));
  EXPECT_EQ(R.Objects, 0u);
  EXPECT_FALSE(R.BudgetExhausted);
  EXPECT_EQ(print(*M), Before);
}
} // namespace

TEST(ByteCellScalarization, PromotesAllOverlapsAcrossEntriesAndBackedges) {
  for (auto Layout : {"e-p:64:64", "E-p:64:64", "e-p:32:32", "E-p:32:32"}) {
    SCOPED_TRACE(Layout);
    llvm::LLVMContext C;
    auto M = parse(C, program(), Layout);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    auto R = ByteCellScalarizationPass::scalarize(F);
    ASSERT_EQ(R.Objects, 1u);
    EXPECT_LT(R.Cells, 24u);
    EXPECT_GT(R.Cells, 3u);
    EXPECT_GT(R.Accesses, 10u);
    EXPECT_FALSE(R.BudgetExhausted);
    ASSERT_FALSE(llvm::verifyFunction(F, &llvm::errs()));
    promote(F);
    EXPECT_EQ(count<llvm::AllocaInst>(F), 0u) << print(*M);
    EXPECT_EQ(count<llvm::LoadInst>(F), 0u);
    EXPECT_EQ(count<llvm::StoreInst>(F), 3u); // Every external output survives.
  }
}

TEST(ByteCellScalarization, DefaultPipelinePromotesRemainingArrays) {
  for (bool Deep : {false, true}) {
    llvm::LLVMContext C;
    auto M = parse(C, program());
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    promote(F);
    ASSERT_GT(count<llvm::AllocaInst>(F), 0u); // Ordinary SROA left overlap.
    Pipeline::OptimizationOptions O;
    O.Strength =
        Deep ? Pipeline::OptStrength::Deep : Pipeline::OptStrength::Thin;
    const auto R = Pipeline::optimizeModule(*M, O);
    EXPECT_NE(R.Stop, OptimizationStopReason::InputInvalid);
    EXPECT_NE(R.Stop, OptimizationStopReason::VerificationFailed);
    // The transactional pipeline can replace the module's function bodies.
    auto &Optimized = *M->getFunction("f");
    EXPECT_EQ(count<llvm::AllocaInst>(Optimized), 0u) << print(*M);
    EXPECT_EQ(count<llvm::LoadInst>(Optimized), 0u);
    EXPECT_GT(count<llvm::StoreInst>(Optimized), 0u);
  }
}

TEST(ByteCellScalarization, DoesNotSplitExactOrDisjointHomes) {
  refuses(R"(
define i128 @f(i128 %x) {
  %a = alloca [32 x i8]
  %b = getelementptr i8, ptr %a, i64 16
  store i128 %x, ptr %a, align 1
  store i128 %x, ptr %b, align 1
  %v = load i128, ptr %a, align 1
  ret i128 %v
})");
}

TEST(ByteCellScalarization, RejectsCompleteEscapingDynamicAndOrderedObjects) {
  for (auto Replacement : {"%p3 = getelementptr i8, ptr %bytes, i64 %y",
                           "%p3 = getelementptr i8, ptr %bytes, i64 -1",
                           "%p3 = getelementptr i8, ptr %bytes, i64 20"}) {
    auto S = program();
    replace(S, "%p3 = getelementptr i8, ptr %bytes, i64 3", Replacement);
    refuses(S);
  }
  for (auto Replacement : {"store volatile i64 %x, ptr %bytes, align 1",
                           "store atomic i64 %x, ptr %bytes monotonic, align 8",
                           "store ptr %bytes, ptr %out, align 1"}) {
    auto S = program();
    replace(S, "store i64 %x, ptr %bytes, align 1", Replacement);
    refuses(S);
  }
  auto S = program();
  replace(S, "%old = load i64, ptr %p3, align 1",
          "%old = load volatile i64, ptr %p3, align 1");
  refuses(S);
  S = program();
  replace(S, "%p1 =", "%escape = ptrtoint ptr %bytes to i64\n%p1 =");
  refuses(S);
}

TEST(ByteCellScalarization, RejectsMetadataAndStorageContracts) {
  auto S = program();
  replace(S, "store i64 %x, ptr %bytes, align 1",
          "store i64 %x, ptr %bytes, align 1, !example !0");
  refuses(S + "\n!0 = !{i32 9}\n");
  S = program();
  replace(S, "alloca [24 x i8], align 8", "alloca [24 x i8], i64 2, align 8");
  refuses(S);
  S = program();
  replace(S, "alloca [24 x i8], align 8", "alloca inalloca [24 x i8], align 8");
  refuses(S);
  S = program();
  replace(S, "ptr %out) {", "ptr %out) \"neverd-obfuscated\" {");
  refuses(S);
  refuses(program(), "e-p:64:64:64:32");
  refuses(program(), "e-p:64:64-A5");
}

TEST(ByteCellScalarization, SharesArithmeticAndStackObserverCallBoundary) {
  auto S = "declare i64 @llvm.ctpop.i64(i64)\n" + program();
  replace(S, "%seed = xor i64 %x, %y",
          "%seed = call i64 @llvm.ctpop.i64(i64 %x)");
  llvm::LLVMContext C;
  auto M = parse(C, S);
  ASSERT_TRUE(M);
  EXPECT_EQ(ByteCellScalarizationPass::scalarize(*M->getFunction("f")).Objects,
            1u);
  EXPECT_NE(print(*M).find("call i64 @llvm.ctpop.i64"), std::string::npos);
  replace(S, "@llvm.ctpop.i64(i64 %x)",
          "@llvm.ctpop.i64(i64 %x) [ \"deopt\"(i64 %x) ]");
  refuses(S);
  for (auto Name : {"llvm.frameaddress.p0", "llvm.returnaddress"}) {
    S = "declare ptr @" + std::string(Name) + "(i32 immarg)\n" + program();
    replace(S, "%p1 =",
            "%observe = call ptr @" + std::string(Name) + "(i32 0)\n%p1 =");
    refuses(S);
  }
  S = "declare void @unknown() memory(none) nounwind willreturn\n" + program();
  replace(S, "done:", "done:\ncall void @unknown()");
  refuses(S);
}

TEST(ByteCellScalarization, UsesStoredValueOnceWithoutFreezingOrDroppingFlags) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i64 @f(i64 %x) {
  %a = alloca [12 x i8]
  %p = getelementptr i8, ptr %a, i64 3
  %v = add nsw i64 %x, undef
  store i64 %v, ptr %a, align 1
  store i64 21, ptr %p, align 1
  %r = load i64, ptr %a, align 1
  ret i64 %r
})");
  ASSERT_TRUE(M);
  auto &F = *M->getFunction("f");
  llvm::Instruction *Value = nullptr;
  for (auto &I : llvm::instructions(F))
    if (I.getName() == "v")
      Value = &I;
  ASSERT_TRUE(Value);
  ASSERT_EQ(ByteCellScalarizationPass::scalarize(F).Objects, 1u);
  EXPECT_EQ(Value->getNumUses(), 1u);
  EXPECT_TRUE(Value->hasNoSignedWrap());
  EXPECT_TRUE(llvm::isa<llvm::StoreInst>(*Value->user_begin()));
  EXPECT_EQ(count<llvm::FreezeInst>(F), 0u);
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
}

TEST(ByteCellScalarization, OverwrittenPoisonBytesDoNotContaminateTheResult) {
  for (auto Layout : {"e-p:64:64", "E-p:64:64"}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
define i16 @f() {
  %a = alloca [8 x i8]
  %p = getelementptr i8, ptr %a, i64 3
  store i64 poison, ptr %a, align 1
  store i16 19027, ptr %p, align 1
  %v = load i16, ptr %p, align 1
  ret i16 %v
})",
                   Layout);
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    ASSERT_EQ(ByteCellScalarizationPass::scalarize(F).Objects, 1u);
    promote(F);
    auto *R = llvm::cast<llvm::ReturnInst>(F.getEntryBlock().getTerminator());
    auto *Value = llvm::dyn_cast<llvm::ConstantInt>(R->getReturnValue());
    ASSERT_TRUE(Value) << print(*M);
    EXPECT_EQ(Value->getZExtValue(), 19027u);
  }
}

TEST(ByteCellScalarization, ExactWorkAndEveryShortCeilingAreTransactional) {
  llvm::LLVMContext C;
  auto M = parse(C, program());
  ASSERT_TRUE(M);
  auto R = ByteCellScalarizationPass::scalarize(*M->getFunction("f"));
  ASSERT_EQ(R.Objects, 1u);
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto Other = parse(C, program());
    ASSERT_TRUE(Other);
    const auto Before = print(*Other);
    ByteCellScalarizationOptions O;
    if (Kind < 2)
      O.MaxWork = R.Work - Kind;
    if (Kind == 2)
      O.MaxInstructions = R.Instructions - 1;
    if (Kind == 3)
      O.MaxCells = R.Cells - 1;
    if (Kind == 4)
      O.MaxNewInstructions = R.NewInstructions - 1;
    if (Kind == 5)
      O.MaxWork = 0;
    auto Next =
        ByteCellScalarizationPass::scalarize(*Other->getFunction("f"), O);
    EXPECT_EQ(Next.Objects, Kind == 0 ? 1u : 0u) << Kind;
    EXPECT_EQ(Next.BudgetExhausted, Kind != 0) << Kind;
    if (Kind)
      EXPECT_EQ(print(*Other), Before) << Kind;
    else
      EXPECT_EQ(print(*Other), print(*M));
  }
}

TEST(ByteCellScalarization, WideAndOddWidthAccessesRespectTargetByteOrder) {
  for (bool BigEndian : {false, true}) {
    llvm::APInt Seed(128, 0);
    unsigned char Bytes[16];
    for (unsigned I = 0; I < 16; ++I) {
      Bytes[I] = I + 1;
      Seed |= llvm::APInt(128, Bytes[I]).shl(8 * (BigEndian ? 15 - I : I));
    }
    for (unsigned I = 0; I < 3; ++I)
      Bytes[5 + I] = (0xa1b2c3u >> (8 * (BigEndian ? 2 - I : I))) & 255;
    uint64_t Expected = 0;
    for (unsigned I = 0; I < 8; ++I)
      Expected |= uint64_t(Bytes[3 + I]) << (8 * (BigEndian ? 7 - I : I));
    llvm::SmallString<64> Decimal;
    Seed.toStringUnsigned(Decimal);
    auto S = R"(
define i64 @f() {
  %a = alloca [16 x i8]
  %p = getelementptr i8, ptr %a, i64 5
  %q = getelementptr i8, ptr %a, i64 3
  store i128 SEED, ptr %a, align 1
  store i24 10597059, ptr %p, align 1
  %v = load i64, ptr %q, align 1
  ret i64 %v
})";
    std::string IR = S;
    replace(IR, "SEED", Decimal);
    llvm::LLVMContext C;
    auto M = parse(C, IR, BigEndian ? "E-p:64:64" : "e-p:32:32");
    ASSERT_TRUE(M);
    auto &F = *M->getFunction("f");
    ASSERT_EQ(ByteCellScalarizationPass::scalarize(F).Objects, 1u);
    Pipeline::OptimizationOptions Options;
    Options.Strength = Pipeline::OptStrength::Thin;
    const auto Result = Pipeline::optimizeModule(*M, Options);
    ASSERT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
    auto *Return = llvm::cast<llvm::ReturnInst>(
        M->getFunction("f")->getEntryBlock().getTerminator());
    auto *Value = llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
    ASSERT_TRUE(Value) << print(*M);
    EXPECT_EQ(Value->getZExtValue(), Expected);
  }
}

TEST(ByteCellScalarization, LateObjectBudgetFailureRollsBackEveryPlan) {
  const std::string S = R"(
define i64 @f(i64 %x) {
  %a = alloca [12 x i8]
  %b = alloca [12 x i8]
  %p = getelementptr i8, ptr %a, i64 3
  %q = getelementptr i8, ptr %b, i64 3
  store i64 %x, ptr %a, align 1
  store i64 %x, ptr %b, align 1
  store i64 72, ptr %p, align 1
  store i64 93, ptr %q, align 1
  %av = load i64, ptr %a, align 1
  %bv = load i64, ptr %b, align 1
  %r = xor i64 %av, %bv
  ret i64 %r
})";
  llvm::LLVMContext C;
  auto M = parse(C, S);
  ASSERT_TRUE(M);
  auto R = ByteCellScalarizationPass::scalarize(*M->getFunction("f"));
  ASSERT_EQ(R.Objects, 2u);
  auto Other = parse(C, S);
  const auto Before = print(*Other);
  ByteCellScalarizationOptions O;
  O.MaxNewInstructions = R.NewInstructions - 1;
  auto Failed =
      ByteCellScalarizationPass::scalarize(*Other->getFunction("f"), O);
  EXPECT_TRUE(Failed.BudgetExhausted);
  EXPECT_EQ(Failed.Objects, 0u);
  EXPECT_EQ(print(*Other), Before);
  auto Escaping = S;
  replace(Escaping, "%r = xor", "%number = ptrtoint ptr %b to i64\n%r = xor");
  auto Mixed = parse(C, Escaping);
  ASSERT_TRUE(Mixed);
  R = ByteCellScalarizationPass::scalarize(*Mixed->getFunction("f"));
  EXPECT_EQ(R.Objects, 1u);
  EXPECT_NE(print(*Mixed).find("store i64 93, ptr %q"), std::string::npos);
  EXPECT_FALSE(llvm::verifyModule(*Mixed, &llvm::errs()));
}
} // namespace neverd::cell_test

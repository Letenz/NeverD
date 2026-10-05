//===- LLVMScalarResultProjectionTests.cpp - Declared observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "neverd/analysis/LLVMScalarResultProjection.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

namespace neverd::analysis::scalar_test {
using Projection = LLVMScalarResultProjectionResult;

static Projection project(Source &S, LLVMScalarResultObservation O,
                          const LLVMScalarResultProjectionLimits &L = {}) {
  const auto Before = S.text();
  auto R = projectLLVMScalarResult(S.function(), O, L);
  EXPECT_EQ(Before, S.text());
  if (R.Module)
    EXPECT_FALSE(llvm::verifyModule(*R.Module));
  return R;
}

TEST(LLVMScalarProjection, NestedFieldsAndWindowsRetainEveryArgument) {
  constexpr char Body[] = R"(
define { { i64, [2 x i32] }, i32 } @f(i32 noundef %x, i32 noundef %y) {
  %a = zext i32 %x to i64
  %b = zext i32 %y to i64
  %high = shl i64 %a, 32
  %joined = or i64 %high, %b
  %extra = add i32 %x, 17
  %s0 = insertvalue { { i64, [2 x i32] }, i32 } zeroinitializer, i64 %joined, 0, 0
  %s1 = insertvalue { { i64, [2 x i32] }, i32 } %s0, i32 %extra, 0, 1, 1
  %s2 = insertvalue { { i64, [2 x i32] }, i32 } %s1, i32 %y, 1
  ret { { i64, [2 x i32] }, i32 } %s2
})";
  for (const char *Triple : {"x86_64-linux-gnu", "aarch64-linux-gnu",
                             "aarch64_be-linux-gnu", "armv7-linux-gnueabihf"}) {
    SCOPED_TRACE(Triple);
    Source S(std::string("target triple = \"") + Triple + "\"\n" + Body);
    ASSERT_TRUE(S.Module);
    const LLVMScalarResultObservation Observations[] = {{{0, 0}, 0, 32},
                                                        {{0, 0}, 32, 32},
                                                        {{0, 1, 1}, 0, 32},
                                                        {{0, 1, 0}, 0, 32}};
    const char *Answers[] = {"%y", "%x", "%sum", "0"};
    for (unsigned I = 0; I < std::size(Observations); ++I) {
      SCOPED_TRACE(I);
      auto R = project(S, Observations[I]);
      ASSERT_EQ(R.Status, Projection::Projected) << R.Diagnostic;
      auto *F = R.Module->getFunction("f");
      ASSERT_TRUE(F);
      EXPECT_EQ(F->arg_size(), 2U);
      EXPECT_EQ(R.RetainedInstructions, 5U);
      EXPECT_EQ(R.RemovedPackaging, 3U);
      Source Oracle(
          std::string("define i32 @f(i32 noundef %x, i32 noundef %y) {") +
          " %sum = add i32 %x, 17\nret i32 " + Answers[I] + " }");
      auto Proof = checkLLVMScalarEquivalence(*F, Oracle.function());
      EXPECT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
    }
  }
}

TEST(LLVMScalarProjection, MultipleReturnsAndBackedgesKeepTheCompleteDomain) {
  Source S(R"(
define { i32, i32 } @f(i32 noundef %x, i8 noundef %n) {
entry:
  %limit = and i8 %n, 7
  %empty = icmp eq i8 %limit, 0
  br i1 %empty, label %zero, label %loop
zero:
  %z = insertvalue { i32, i32 } zeroinitializer, i32 %x, 1
  ret { i32, i32 } %z
loop:
  %i = phi i8 [0, %entry], [%next, %latch]
  %v = phi i32 [%x, %entry], [%sum, %latch]
  %parity = and i8 %i, 1
  %odd = icmp ne i8 %parity, 0
  br i1 %odd, label %a, label %b
a: %va = add i32 %v, 9
  br label %latch
b: %vb = add i32 %v, 3
  br label %latch
latch:
  %sum = phi i32 [%va, %a], [%vb, %b]
  %next = add nuw i8 %i, 1
  %more = icmp ult i8 %next, %limit
  br i1 %more, label %loop, label %exit
exit:
  %s = insertvalue { i32, i32 } zeroinitializer, i32 %sum, 1
  ret { i32, i32 } %s
})");
  auto R = project(S, {{1}, 0, 32});
  ASSERT_EQ(R.Status, Projection::Projected) << R.Diagnostic;
  Source Oracle(R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
  %k = and i8 %n, 7
  %count = zext i8 %k to i32
  %up = add i32 %count, 1
  %even = lshr i32 %up, 1
  %odd = lshr i32 %count, 1
  %a = mul i32 %even, 3
  %b = mul i32 %odd, 9
  %delta = add i32 %a, %b
  %r = add i32 %x, %delta
  ret i32 %r
})");
  auto *F = R.Module->getFunction("f");
  auto Proof = checkLLVMScalarEquivalence(*F, Oracle.function());
  ASSERT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
  EXPECT_EQ(Proof.CompletedPartitions, 8U);
  auto StatusProjection = project(S, {{0}, 0, 32});
  ASSERT_EQ(StatusProjection.Status, Projection::Projected);
  Source Zero("define i32 @f(i32 noundef %x, i8 noundef %n) {ret i32 0}");
  EXPECT_EQ(checkLLVMScalarEquivalence(
                *StatusProjection.Module->getFunction("f"), Zero.function())
                .Status,
            Status::Proved);
  EXPECT_EQ(checkLLVMScalarEquivalence(*F, Zero.function()).Status,
            Status::Unproved);
}

TEST(LLVMScalarProjection, UnselectedArithmeticAndAssumptionsStayObligations) {
  for (const char *Operation :
       {"%bad = add nuw i8 255, 1", "%bad = shl i8 %x, %x",
        "call void @llvm.assume(i1 false)"}) {
    Source S(std::string("declare void @llvm.assume(i1)\n") +
             "define {i8, i8} @f(i8 noundef %x) {\n" + Operation +
             "\n %s = insertvalue {i8, i8} zeroinitializer, i8 %x, 0\n"
             " ret {i8, i8} %s }");
    auto R = project(S, {{0}, 0, 8});
    ASSERT_EQ(R.Status, Projection::Projected) << R.Diagnostic;
    auto &F = *R.Module->getFunction("f");
    EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Unproved);
  }
  Source Infinite(R"(
define {i8, i8} @f(i8 noundef %x) {
entry: br i1 true, label %loop, label %exit
loop: br label %loop
exit: ret {i8, i8} zeroinitializer
})");
  auto R = project(Infinite, {{0}, 0, 8});
  ASSERT_EQ(R.Status, Projection::Projected) << R.Diagnostic;
  LLVMScalarEquivalenceLimits L;
  L.MaxBlockVisits = 4;
  auto &F = *R.Module->getFunction("f");
  EXPECT_EQ(checkLLVMScalarEquivalence(F, F, L).Status, Status::BudgetExceeded);
}

TEST(LLVMScalarProjection, UnsupportedShapesAndContractsPublishNothing) {
  const char *Programs[] = {
      "define {i8,i8} @f(i8 noundef %x) {ret {i8,i8} {i8 0,i8 poison}}",
      "define {i8,i8} @f(i8 noundef %x) {"
      " %s = insertvalue {i8,i8} undef, i8 %x, 0\nret {i8,i8} %s}",
      "define {i8,i8} @f(i8 noundef %x) {"
      " %s = select i1 true, {i8,i8} zeroinitializer, {i8,i8} {i8 1,i8 2}\n"
      " ret {i8,i8} %s}",
      "define {i8,i8} @f(i8 noundef %x) {br label %b\nb:"
      " %p = phi {i8,i8} [zeroinitializer, %0]\nret {i8,i8} %p}",
      "define {i8,i8} @f(ptr noundef %x) {"
      " %v = load i8, ptr %x\nret {i8,i8} zeroinitializer}",
      "declare i8 @g(i8)\ndefine {i8,i8} @f(i8 noundef %x) {"
      " %v = call i8 @g(i8 %x)\nret {i8,i8} zeroinitializer}",
      "define {i8,i8} @f(i8 noundef %x) \"unknown-contract\" {"
      "ret {i8,i8} zeroinitializer}",
      "$group = comdat any\n"
      "define linkonce_odr {i8,i8} @f(i8 noundef %x) comdat($group) {"
      "ret {i8,i8} zeroinitializer}",
      "define {i8,i8} @f(i8 noundef %x) {"
      " %s = insertvalue {i8,i8} zeroinitializer, i8 %x, 0\n"
      " %e = extractvalue {i8,i8} %s, 1\nret {i8,i8} %s}"};
  for (const char *IR : Programs) {
    SCOPED_TRACE(IR);
    Source S(IR);
    ASSERT_TRUE(S.Module);
    auto R = project(S, {{0}, 0, 8});
    EXPECT_EQ(R.Status, Projection::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.Diagnostic.empty());
    EXPECT_FALSE(R.Module);
  }
  Source S("define range(i8 0, 4) i8 @f(i8 noundef %x) {ret i8 %x}");
  EXPECT_EQ(project(S, {{}, 0, 8}).Status, Projection::Unsupported);
}

TEST(LLVMScalarProjection, WindowsAndExactConstructionBudgetsAreChecked) {
  Source S(R"(
define {i64, i8} @f(i64 noundef %x) {
  %s = insertvalue {i64, i8} zeroinitializer, i64 %x, 0
  ret {i64, i8} %s
})");
  const LLVMScalarResultObservation Valid{{0}, 32, 32};
  auto R = project(S, Valid);
  ASSERT_EQ(R.Status, Projection::Projected) << R.Diagnostic;
  LLVMScalarResultProjectionLimits L;
  L.MaxConstructionWork = R.ConstructionWork;
  EXPECT_EQ(project(S, Valid, L).Status, Projection::Projected);
  --L.MaxConstructionWork;
  auto Short = project(S, Valid, L);
  EXPECT_EQ(Short.Status, Projection::BudgetExceeded);
  EXPECT_FALSE(Short.Module);
  L = {};
  L.Model.MaxWork = 0;
  auto ModelShort = project(S, Valid, L);
  EXPECT_EQ(ModelShort.Status, Projection::BudgetExceeded);
  EXPECT_FALSE(ModelShort.Module);
  const LLVMScalarResultObservation Bad[] = {
      {{2}, 0, 8},  {{0, 0}, 0, 8}, {{0}, 64, 1}, {{0}, 60, 8},
      {{1}, 0, 16}, {{}, 0, 8},     {{0}, 0, 7}};
  for (auto O : Bad) {
    auto Invalid = project(S, O);
    EXPECT_EQ(Invalid.Status, Projection::Unsupported);
    EXPECT_FALSE(Invalid.Module);
  }
}

TEST(LLVMScalarProjection, ReprojectionChecksChangedUnselectedOperations) {
  Source S(R"(
define {i8, i8} @f(i8 noundef %x) {
  %dead = add nuw i8 %x, 0
  %s = insertvalue {i8, i8} zeroinitializer, i8 %x, 0
  ret {i8, i8} %s
})");
  auto First = project(S, {{0}, 0, 8});
  ASSERT_EQ(First.Status, Projection::Projected);
  auto &F = *First.Module->getFunction("f");
  EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Proved);
  S.function().front().front().setOperand(
      1, llvm::ConstantInt::get(llvm::Type::getInt8Ty(S.Context), 1));
  auto Changed = project(S, {{0}, 0, 8});
  ASSERT_EQ(Changed.Status, Projection::Projected);
  auto &G = *Changed.Module->getFunction("f");
  EXPECT_EQ(checkLLVMScalarEquivalence(G, G).Status, Status::Unproved);
  EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Proved);
}

TEST(LLVMScalarProjection, LastPartitionObligationsAndReturnMetadataRemain) {
  Source S(R"(
declare void @llvm.assume(i1)
define {i8, i8} @f(i8 noundef %n) {
  %count = and i8 %n, 7
  %ok = icmp ne i8 %count, 7
  call void @llvm.assume(i1 %ok)
  ret {i8, i8} zeroinitializer
})");
  auto R = project(S, {{1}, 0, 8});
  ASSERT_EQ(R.Status, Projection::Projected) << R.Diagnostic;
  auto &F = *R.Module->getFunction("f");
  EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Unproved);
  Source ConstantShift(R"(
define {i8, i8} @f(i8 noundef %x) {
  %bad = lshr i8 %x, 8
  ret {i8, i8} zeroinitializer
})");
  EXPECT_EQ(project(ConstantShift, {{1}, 0, 8}).Status,
            Projection::Unsupported);
  Source Unknown(R"(
define {i8, i8} @f(i8 noundef %x) {
  ret {i8, i8} zeroinitializer, !unknown_contract !0
}
!0 = !{i32 1}
)");
  EXPECT_EQ(project(Unknown, {{1}, 0, 8}).Status, Projection::Unsupported);
}

class LLVMScalarProjectionCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarProjectionCompiled, OriginalAggregateAndWindowsMatchOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  Source S(R"(
define {i64, i32} @f(i32 noundef %x, i32 noundef %n) {
entry:
  %limit = and i32 %n, 7
  br label %head
head:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = phi i32 [%x, %entry], [%sum, %body]
  %more = icmp ult i32 %i, %limit
  br i1 %more, label %body, label %exit
body:
  %sum = add i32 %v, 11
  %next = add nuw nsw i32 %i, 1
  br label %head
exit:
  %lo = zext i32 %v to i64
  %wide = zext i32 %x to i64
  %hi = shl i64 %wide, 32
  %packed = or i64 %hi, %lo
  %s = insertvalue {i64, i32} zeroinitializer, i64 %packed, 0
  ret {i64, i32} %s
})");
  ASSERT_TRUE(S.Module);
  auto Low = project(S, {{0}, 0, 32});
  auto High = project(S, {{0}, 32, 32});
  auto StatusField = project(S, {{1}, 0, 32});
  ASSERT_EQ(Low.Status, Projection::Projected) << Low.Diagnostic;
  ASSERT_EQ(High.Status, Projection::Projected) << High.Diagnostic;
  ASSERT_EQ(StatusField.Status, Projection::Projected)
      << StatusField.Diagnostic;
  const auto Original = tmpFile("aggregate.ll");
  auto Text = S.text();
  Text.replace(Text.find("@f("), 3, "@body(");
  std::ofstream(Original) << Text << R"(
define void @original(i32 %x, i32 %n, ptr %out) {
  %s = call {i64, i32} @body(i32 %x, i32 %n)
  %v = extractvalue {i64, i32} %s, 0
  %status = extractvalue {i64, i32} %s, 1
  %wide = zext i32 %status to i64
  store i64 %v, ptr %out
  %p = getelementptr i64, ptr %out, i64 1
  store i64 %wide, ptr %p
  ret void
})";
  std::vector<std::string> Paths;
  for (auto [R, Name] :
       {std::pair{&Low, "low"}, {&High, "high"}, {&StatusField, "status"}}) {
    auto &F = *R->Module->getFunction("f");
    ASSERT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Proved);
    F.setName(Name);
    auto Path = tmpFile(std::string(Name) + ".ll");
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    R->Module->print(OS, nullptr);
    std::ofstream(Path) << IR;
    Paths.push_back(Path.string());
  }
  const auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
void original(uint32_t, uint32_t, uint64_t *);
uint32_t low(uint32_t, uint32_t), high(uint32_t, uint32_t), status(uint32_t, uint32_t);
int main(void) {
  const uint32_t edges[] = {0, 1, UINT32_MAX, UINT32_MAX - 32, 0x7fffffff, 0x80000000};
  uint32_t random = 0x392ad785;
  for (unsigned k = 0; k < 8192; ++k) {
    random = random * 1664525u + 1013904223u;
    uint32_t x = k < 6 * 256 ? edges[k / 256] : random;
    uint32_t n = k < 6 * 256 ? k : random >> 8;
    uint32_t sum = x + 11u * (n & 7);
    uint64_t observed[2]; original(x, n, observed);
    if (observed[0] != (((uint64_t)x << 32) | sum) || observed[1] != 0) return 1;
    if (low(x, n) != sum || high(x, n) != x || status(x, n) != 0) return 2;
  }
  return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    const auto Program = tmpFile("projection-oracle");
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
              Original.string(), Paths[0], Paths[1], Paths[2], Harness.string(),
              "-o", Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}
} // namespace neverd::analysis::scalar_test

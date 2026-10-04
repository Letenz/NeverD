//===- LLVMScalarDecisionTests.cpp - Demand-driven scalar decisions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace neverd::analysis::scalar_test {
namespace {
// A bounded word recurrence, independently authored for the scalar model.
// Repeated exact shifts and extension guards build a deep definedness DAG.
// All data bits remain symbolic, including the high bits of the final XOR.
constexpr char Recurrence[] = R"(
define i64 @f(i64 noundef %x, i32 noundef %n) {
entry:
  %limit = and i32 %n, 15
  %seed = and i64 %x, 16777215
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = phi i64 [%seed, %entry], [%sum, %body]
  %go = icmp ult i32 %i, %limit
  br i1 %go, label %body, label %exit
body:
  %masked = and i64 %v, 1073741760
  %shifted = lshr exact i64 %masked, 6
  %small = trunc nuw nsw i64 %shifted to i32
  %wide = zext nneg i32 %small to i64
  %sum = add nuw nsw i64 %wide, %seed
  %next = add nuw nsw i32 %i, 1
  br label %loop
exit:
  %result = xor i64 %v, %x
  ret i64 %result
})";

void replace(std::string &IR, llvm::StringRef From, llvm::StringRef To) {
  auto At = IR.find(From.str());
  ASSERT_NE(At, std::string::npos);
  IR.replace(At, From.size(), To.str());
}

LLVMScalarEquivalenceResult
self(llvm::StringRef IR, const LLVMScalarEquivalenceLimits &Limits = {}) {
  Source Input(IR);
  const auto Before = Input.text();
  auto Result =
      checkLLVMScalarEquivalence(Input.function(), Input.function(), Limits);
  EXPECT_EQ(Input.text(), Before);
  return Result;
}

std::string twoBackedges() {
  std::string IR = Recurrence;
  replace(IR, "[%next, %body]", "[%next, %odd], [%next, %even]");
  replace(IR, "[%sum, %body]", "[%extra, %odd], [%sum, %even]");
  replace(IR, "  br label %loop\nexit:", R"(
  %bit = and i32 %i, 1
  %turn = icmp eq i32 %bit, 0
  br i1 %turn, label %even, label %odd
odd:
  %extra = add nuw nsw i64 %sum, 19
  br label %loop
even:
  br label %loop
exit:)");
  return IR;
}
} // namespace

TEST(LLVMScalarDecision, DeepDefinednessRetainsTheCompleteControlDomain) {
  const auto R = self(Recurrence);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits, (std::vector<LLVMScalarControlBit>{
                               {1, 0}, {1, 1}, {1, 2}, {1, 3}}));
  const auto Paired = check(Recurrence, Recurrence);
  ASSERT_EQ(Paired.Status, Status::Proved) << Paired.Diagnostic;
  EXPECT_EQ(Paired.ControlBits, R.ControlBits);
  EXPECT_LT(R.Work, Paired.Work);

  std::string Wrong = Recurrence;
  replace(Wrong, "ret i64 %result", R"(
  %high = and i64 %x, -9223372036854775808
  %wrong = xor i64 %result, %high
  ret i64 %wrong)");
  EXPECT_EQ(check(Recurrence, Wrong).Status, Status::Unproved);
}

TEST(LLVMScalarDecision, ResolvesDeepBranchDecisionsWithoutDataEnumeration) {
  std::string IR = Recurrence;
  replace(IR, "  ret i64 %result", R"(
  %bounded = icmp ult i64 %v, 33554432
  %shift = select i1 %bounded, i64 0, i64 1
  %same = shl i64 %x, %shift
  %equal = icmp eq i64 %same, %x
  br i1 %equal, label %good, label %bad
good:
  ret i64 %result
bad:
  %poison = add nuw i8 255, 1
  ret i64 0)");
  const auto R = self(IR);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits.size(), 4U);
  EXPECT_EQ(check(Recurrence, IR).Status, Status::Proved);
}

TEST(LLVMScalarDecision,
     BothBackedgesAndLateUndefinedOperationsRemainRequired) {
  const auto IR = twoBackedges();
  const auto R = self(IR);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits.size(), 4U);
  EXPECT_EQ(check(IR, IR).Status, Status::Proved);

  std::string Bad = IR;
  replace(Bad, "  ret i64 %result", R"(
  %last = icmp eq i32 %limit, 15
  br i1 %last, label %bad, label %good
bad:
  %unused = add nuw i8 255, 1
  ret i64 %result
good:
  ret i64 %result)");
  const auto Refused = self(Bad);
  EXPECT_EQ(Refused.Status, Status::Unproved);
  EXPECT_EQ(Refused.CompletedPartitions, 15U);
  EXPECT_NE(Refused.Diagnostic.find("not defined"), std::string::npos);

  replace(Bad, "  %unused = add nuw i8 255, 1\n  ret i64 %result",
          "  br label %bad");
  LLVMScalarEquivalenceLimits L;
  L.MaxBlockVisits = 64;
  EXPECT_EQ(self(Bad, L).Status, Status::BudgetExceeded);
}

TEST(LLVMScalarDecision, RefinementChargesExactAndShortWorkAndRetainsCeilings) {
  const auto Baseline = self(Recurrence);
  ASSERT_EQ(Baseline.Status, Status::Proved) << Baseline.Diagnostic;
  LLVMScalarEquivalenceLimits L;
  L.MaxWork = Baseline.Work;
  const auto Exact = self(Recurrence, L);
  EXPECT_EQ(Exact.Status, Status::Proved) << Exact.Diagnostic;
  EXPECT_EQ(Exact.Work, L.MaxWork);
  EXPECT_FALSE(Exact.WorkLimitExceeded);
  --L.MaxWork;
  const auto Short = self(Recurrence, L);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_EQ(Short.Work, L.MaxWork);
  EXPECT_TRUE(Short.WorkLimitExceeded);

  for (unsigned Which = 0; Which != 4; ++Which) {
    SCOPED_TRACE(Which);
    L = {};
    if (Which == 0)
      L.MaxControlBits = 3;
    else if (Which == 1)
      L.MaxPartitions = 15;
    else if (Which == 2)
      L.MaxBlockVisits = 8;
    else
      L.MaxSymbolicNodes = 80;
    const auto Refused = self(Recurrence, L);
    EXPECT_EQ(Refused.Status, Status::BudgetExceeded);
    EXPECT_FALSE(Refused.WorkLimitExceeded);
  }
}

TEST(LLVMScalarDecision, MaskAndAnnotationChangesCannotReuseEarlierFacts) {
  Source Input(Recurrence);
  ASSERT_EQ(
      checkLLVMScalarEquivalence(Input.function(), Input.function()).Status,
      Status::Proved);
  // A mask that retains discarded shift bits makes `exact` invalid for some
  // inputs. A prior proof of the same function object grants no authority.
  for (auto &B : Input.function())
    for (auto &I : B)
      if (I.getName() == "masked")
        I.setOperand(1, llvm::ConstantInt::get(I.getType(), 1073741823));
  EXPECT_NE(
      checkLLVMScalarEquivalence(Input.function(), Input.function()).Status,
      Status::Proved);
}

class LLVMScalarDecisionCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarDecisionCompiled, DeepOneAndTwoBackedgeOracles) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (bool Two : {false, true}) {
    SCOPED_TRACE(Two);
    const std::string IR = Two ? twoBackedges() : Recurrence;
    ASSERT_EQ(self(IR).Status, Status::Proved);
    const auto Source = tmpFile("recurrence.ll");
    const auto Harness = tmpFile("oracle.c");
    std::ofstream(Source) << IR;
    std::ofstream(Harness) << R"(
#include <stdint.h>
uint64_t f(uint64_t, uint32_t);
static uint64_t next_word(uint64_t *s) {
  *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
  return *s;
}
int main(void) {
  uint64_t state = UINT64_C(0x4c718e9ad562b30f);
  const uint64_t edges[] = {0, 1, 63, 64, 16777215, 16777216,
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
  for (unsigned k = 0; k < 8192; ++k) {
    uint64_t x = k < 256 * 9 ? edges[k / 256] : next_word(&state);
    uint32_t n = k < 256 * 9 ? k % 256 : (uint32_t)next_word(&state);
    uint64_t seed = x & UINT64_C(0xffffff), v = seed;
    for (uint32_t i = 0; i < (n & 15); ++i) {
      v = ((v >> 6) & UINT64_C(0xffffff)) + seed;
      if (TWO && (i & 1)) v += 19;
    }
    if (f(x, n) != (v ^ x)) return 1;
  }
  return 0;
})";
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      auto Program = tmpFile("oracle");
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {"-std=c11", Optimization, Two ? "-DTWO=1" : "-DTWO=0",
                "-fsanitize=undefined", "-fsanitize-trap=undefined",
                Source.string(), Harness.string(), "-o", Program.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      const auto Ran = exec(Program.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err;
    }
  }
}
} // namespace neverd::analysis::scalar_test

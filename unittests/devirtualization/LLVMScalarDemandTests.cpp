//===- LLVMScalarDemandTests.cpp - Demanded scalar observations
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

namespace neverd::analysis::scalar_test {
namespace {
// Independently authored nonlinear recurrence. Eager per-write folding
// exhausts a million work units before all sixteen control partitions.
std::string recurrence() {
  std::string IR = R"(
define i32 @f(i32 noundef %x, i32 noundef %n) {
entry:
  %limit = and i32 %n, 15
  br label %head
head:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = phi i32 [%x, %entry], [%result, %body]
  %go = icmp ult i32 %i, %limit
  br i1 %go, label %body, label %exit
body:
)";
  std::string Previous = "%v";
  for (unsigned I = 0; I < 8; ++I) {
    auto N = std::to_string(I);
    IR += "%mul" + N + " = mul i32 " + Previous + ", 13\n";
    IR += "%shift" + N + " = lshr i32 " + Previous + ", 5\n";
    IR += "%mix" + N + " = xor i32 %shift" + N + ", %x\n";
    IR += "%value" + N + " = add i32 %mul" + N + ", %mix" + N + "\n";
    Previous = "%value" + N;
  }
  IR += "%result = xor i32 " + Previous + R"(, %i
  %next = add nuw nsw i32 %i, 1
  br label %head
exit:
  ret i32 %v
})";
  return IR;
}

void returnBody(std::string &IR, llvm::StringRef Body) {
  auto At = IR.find("ret i32 %v");
  ASSERT_NE(At, std::string::npos);
  IR.replace(At, std::string("ret i32 %v").size(), Body.str());
}

std::string separateBody() {
  auto IR = recurrence();
  returnBody(IR, "%dead = xor i32 %x, -1\n"
                 "%copy = add i32 %v, 0\nret i32 %copy");
  return IR;
}
} // namespace

TEST(LLVMScalarDemand, SeparateBodiesKeepEveryNonlinearDataBit) {
  Source Original(recurrence()), Candidate(separateBody());
  auto Before = Original.text(), OtherBefore = Candidate.text();
  LLVMScalarEquivalenceLimits Limits;
  Limits.MaxWork = 65536;
  auto R = checkLLVMScalarEquivalence(Original.function(), Candidate.function(),
                                      Limits);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 16U);
  EXPECT_EQ(R.ControlBits, (std::vector<LLVMScalarControlBit>{
                               {1, 0}, {1, 1}, {1, 2}, {1, 3}}));
  EXPECT_LT(R.Work, Limits.MaxWork);
  EXPECT_EQ(Original.text(), Before);
  EXPECT_EQ(Candidate.text(), OtherBefore);
}

TEST(LLVMScalarDemand, LastPartitionAndFreeHighInputsRemainObserved) {
  auto Original = recurrence(), Last = Original;
  returnBody(Last, "%last = icmp eq i32 %limit, 15\n"
                   "%wrong = xor i32 %v, 1\n"
                   "%answer = select i1 %last, i32 %wrong, i32 %v\n"
                   "ret i32 %answer");
  auto R = check(Original, Last);
  EXPECT_EQ(R.Status, Status::Unproved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 15U);
  for (const char *Input : {"%x", "%n"}) {
    auto Bad = Original;
    returnBody(Bad, std::string("%high = and i32 ") + Input +
                        ", -2147483648\n"
                        "%wrong = xor i32 %v, %high\nret i32 %wrong");
    EXPECT_EQ(check(Original, Bad).Status, Status::Unproved);
  }
}

TEST(LLVMScalarDemand, UnobservedOperationsStillContributeDefinedness) {
  auto Original = recurrence(), Last = Original;
  returnBody(Last, "%last = icmp eq i32 %limit, 15\n"
                   "%amount = select i1 %last, i32 32, i32 0\n"
                   "%dead = shl i32 %x, %amount\nret i32 %v");
  auto R = check(Original, Last);
  EXPECT_EQ(R.Status, Status::Unproved);
  EXPECT_EQ(R.CompletedPartitions, 15U);
  EXPECT_NE(R.Diagnostic.find("not defined"), std::string::npos);
  auto Poison = Original;
  returnBody(Poison, "%dead = add nuw i32 %x, 1\nret i32 %v");
  EXPECT_NE(check(Original, Poison).Status, Status::Proved);
  EXPECT_NE(check(Poison, Original).Status, Status::Proved);
}

TEST(LLVMScalarDemand, BothExecutionsShareExactWorkAndNodeLimits) {
  auto Original = recurrence(), Candidate = separateBody();
  auto Base = check(Original, Candidate);
  ASSERT_EQ(Base.Status, Status::Proved) << Base.Diagnostic;
  LLVMScalarEquivalenceLimits Limits;
  Limits.MaxWork = Base.Work;
  EXPECT_EQ(check(Original, Candidate, Limits).Status, Status::Proved);
  --Limits.MaxWork;
  auto Short = check(Original, Candidate, Limits);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_TRUE(Short.WorkLimitExceeded);
  EXPECT_EQ(Short.Work, Limits.MaxWork);
  Limits = {};
  Limits.MaxSymbolicNodes = 32;
  auto Nodes = check(Original, Candidate, Limits);
  EXPECT_EQ(Nodes.Status, Status::BudgetExceeded);
  EXPECT_FALSE(Nodes.WorkLimitExceeded);
  EXPECT_NE(Nodes.Diagnostic.find("symbolic-node"), std::string::npos);
}

class LLVMScalarDemandCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarDemandCompiled,
       NonlinearRecurrenceMatchesIndependentO0O2Oracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  auto OriginalIR = recurrence(), CandidateIR = separateBody();
  auto Proof = check(OriginalIR, CandidateIR);
  ASSERT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
  OriginalIR.replace(OriginalIR.find("@f("), 3, "@original(");
  CandidateIR.replace(CandidateIR.find("@f("), 3, "@candidate(");
  auto Original = tmpFile("original.ll"), Candidate = tmpFile("candidate.ll");
  std::ofstream(Original) << OriginalIR;
  std::ofstream(Candidate) << CandidateIR;
  auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
uint32_t original(uint32_t, uint32_t);
uint32_t candidate(uint32_t, uint32_t);
int main(void) {
  uint32_t random = 0x12367u;
  const uint32_t edges[] = {0, 1, UINT32_MAX, UINT32_MAX-15, 0x80000000, 0x7fffffff};
  for (unsigned k=0; k<65536; ++k) {
    random = random*1664525u+1013904223u;
    uint32_t x = k<1536 ? edges[k/256] : random;
    uint32_t n = (random & ~255u) | (k & 255u);
    uint32_t expected = x;
    for (unsigned i=0; i<(n&15u); ++i) {
      for (unsigned j=0; j<8; ++j)
        expected = expected*13u + ((expected>>5)^x);
      expected ^= i;
    }
    if (original(x,n)!=expected || candidate(x,n)!=expected) return 1;
  }
  return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Program = tmpFile("demand-oracle");
    auto Built =
        exec(NEVERD_TEST_CLANG,
             {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
              Original.string(), Candidate.string(), Harness.string(), "-o",
              Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}
} // namespace neverd::analysis::scalar_test

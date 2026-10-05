//===- LLVMScalarAssumeTests.cpp - Checked assumption obligations --------===//
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
constexpr char AssumedLoop[] = R"(
declare void @llvm.assume(i1)
define i64 @f(i64 noundef %x, i8 noundef %n) {
entry:
  %limit = and i8 %n, 7
  br label %head
head:
  %i = phi i8 [0, %entry], [%next, %body]
  %v = phi i64 [%x, %entry], [%sum, %body]
  %more = icmp ult i8 %i, %limit
  br i1 %more, label %body, label %exit
body:
  call void @llvm.assume(i1 noundef %more)
  %sum = add i64 %v, 5
  %next = add nuw nsw i8 %i, 1
  br label %head
exit:
  ret i64 %v
})";
constexpr char Reference[] = R"(
define i64 @f(i64 noundef %x, i8 noundef %n) {
  %limit = and i8 %n, 7
  %wide = zext i8 %limit to i64
  %delta = mul i64 %wide, 5
  %sum = add i64 %x, %delta
  ret i64 %sum
})";
} // namespace

TEST(LLVMScalarAssume, GuardedLoopsKeepTheCompleteInputDomain) {
  const auto R = check(AssumedLoop, Reference);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 8U);
  EXPECT_EQ(R.ControlBits,
            (std::vector<LLVMScalarControlBit>{{1, 0}, {1, 1}, {1, 2}}));
  EXPECT_EQ(check(Reference, AssumedLoop).Status, Status::Proved);
  for (const char *Triple : {"x86_64-linux-gnu", "aarch64-linux-gnu",
                             "aarch64_be-linux-gnu", "armv7-linux-gnueabihf"}) {
    SCOPED_TRACE(Triple);
    std::string Input = "target triple = \"" + std::string(Triple) + "\"\n";
    Input += AssumedLoop;
    EXPECT_EQ(check(Input, Reference).Status, Status::Proved);
  }
}

TEST(LLVMScalarAssume, LastPartitionFailureCannotNarrowTheDomain) {
  std::string Bad = AssumedLoop;
  auto At = Bad.find("call void @llvm.assume(i1 noundef %more)");
  Bad.replace(At,
              std::string("call void @llvm.assume(i1 noundef %more)").size(),
              "%ok = icmp ne i8 %limit, 7\n"
              "call void @llvm.assume(i1 %ok)");
  for (const auto &R : {check(Bad, Reference), check(Reference, Bad)}) {
    EXPECT_EQ(R.Status, Status::Unproved) << R.Diagnostic;
    EXPECT_EQ(R.CompletedPartitions, 7U);
  }
  Source S(Bad);
  auto Self = checkLLVMScalarEquivalence(S.function(), S.function());
  EXPECT_EQ(Self.Status, Status::Unproved);
  EXPECT_EQ(Self.CompletedPartitions, 7U);
}

TEST(LLVMScalarAssume, EveryReachedConditionAccumulatesDefinedness) {
  Source S(R"(
declare void @llvm.assume(i1)
define i8 @f(i8 noundef %x) {
  %bits = and i8 %x, 3
  %first = icmp ne i8 %bits, 3
  %second = icmp ult i8 %x, 200
  call void @llvm.assume(i1 %first)
  call void @llvm.assume(i1 %second)
  %result = xor i8 %x, 165
  ret i8 %result
})");
  ASSERT_TRUE(S.Module);
  const auto Before = S.text();
  auto M = modelLLVMScalarFunction(S.function());
  ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
  for (unsigned X = 0; X != 256; ++X) {
    auto R = evaluate(*M, {llvm::APInt(8, X)}, (X & 3) != 3 && X < 200);
    ASSERT_TRUE(R);
    EXPECT_EQ(R->getZExtValue(), X ^ 165);
  }
  EXPECT_EQ(S.text(), Before);
}

TEST(LLVMScalarAssume, CallContractsAndPoisonOperandsStayExplicit) {
  for (const char *Call :
       {"call void @llvm.assume(i1 undef)", "call void @llvm.assume(i1 poison)",
        "call fastcc void @llvm.assume(i1 true)",
        "call void @llvm.assume(i1 true) [\"cold\"()]",
        "call void @llvm.assume(i1 true) [\"noundef\"(i8 %x)]",
        "call void @llvm.assume(i1 true) convergent",
        "call void @llvm.assume(i1 \"unknown\" true)"}) {
    SCOPED_TRACE(Call);
    std::string IR = "declare void @llvm.assume(i1)\n"
                     "define i8 @f(i8 noundef %x) {\n";
    IR += Call;
    IR += "\nret i8 %x }";
    EXPECT_EQ(check(IR, IR).Status, Status::Unsupported);
  }
  Source Extra(R"(
declare void @llvm.assume(i1)
define i8 @f(i8 noundef %x) {
  call void @llvm.assume(i1 true)
  ret i8 %x
})");
  ASSERT_TRUE(Extra.Module);
  // Parsing may canonicalize intrinsic attributes; change the actual IR
  // declaration so this checks model admission, not parser upgrading.
  Extra.Module->getFunction("llvm.assume")->addFnAttr("unknown-contract");
  EXPECT_EQ(
      checkLLVMScalarEquivalence(Extra.function(), Extra.function()).Status,
      Status::Unsupported);
}

TEST(LLVMScalarAssume, UnreachableFalseDiffersFromReachedUndefinedness) {
  constexpr char Unreachable[] = R"(
declare void @llvm.assume(i1)
define i8 @f(i8 noundef %x) {
entry: br i1 false, label %bad, label %good
bad:
  call void @llvm.assume(i1 false)
  ret i8 0
good: ret i8 %x
})";
  constexpr char Identity[] = "define i8 @f(i8 noundef %x) { ret i8 %x }";
  EXPECT_EQ(check(Unreachable, Identity).Status, Status::Proved);
  std::string Reached = Unreachable;
  Reached.replace(Reached.find("br i1 false"), 11, "br i1 true");
  EXPECT_EQ(check(Reached, Identity).Status, Status::Unproved);
  constexpr char Poison[] = R"(
declare void @llvm.assume(i1)
define i8 @f(i8 noundef %x) {
  %bad = add nuw i8 255, 1
  call void @llvm.assume(i1 true)
  ret i8 %x
})";
  EXPECT_EQ(check(Poison, Identity).Status, Status::Unproved);
  constexpr char Infinite[] = R"(
declare void @llvm.assume(i1)
define i8 @f(i8 noundef %x) {
entry: br label %loop
loop:
  call void @llvm.assume(i1 false)
  br label %loop
})";
  LLVMScalarEquivalenceLimits L;
  L.MaxBlockVisits = 4;
  EXPECT_NE(check(Infinite, Identity, L).Status, Status::Proved);
}

TEST(LLVMScalarAssume, WorkLimitsAndMutationsCannotReuseAProof) {
  Source S(AssumedLoop);
  const auto Before = S.text();
  auto Query = [&](const LLVMScalarEquivalenceLimits &L = {}) {
    return checkLLVMScalarEquivalence(S.function(), S.function(), L);
  };
  const auto R = Query();
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  LLVMScalarEquivalenceLimits L;
  L.MaxWork = R.Work;
  EXPECT_EQ(Query(L).Status, Status::Proved);
  --L.MaxWork;
  auto Short = Query(L);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_TRUE(Short.WorkLimitExceeded);
  EXPECT_EQ(Short.Work, L.MaxWork);
  L = {};
  L.Model.MaxOperations = 0;
  EXPECT_EQ(Query(L).Status, Status::BudgetExceeded);
  EXPECT_EQ(S.text(), Before);
  for (auto &B : S.function())
    for (auto &I : B)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
        Call->setArgOperand(0, llvm::ConstantInt::getFalse(S.Context));
  EXPECT_EQ(Query().Status, Status::Unproved);
}

class LLVMScalarAssumeCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarAssumeCompiled, LoopMatchesIndependentUnsignedArithmetic) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  auto Source = tmpFile("assumed-loop.ll"),
       Harness = tmpFile("assume-oracle.c");
  std::ofstream(Source) << AssumedLoop;
  std::ofstream(Harness) << R"(
#include <stdint.h>
uint64_t f(uint64_t, uint8_t);
int main(void) {
  uint64_t state = UINT64_C(0x7c25ab4ef83619d0);
  const uint64_t edges[] = {0, 1, UINT64_MAX, UINT64_MAX - 7,
    UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000)};
  for (unsigned k = 0; k < 4096; ++k) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    uint64_t x = k < 6 * 256 ? edges[k / 256] : state;
    uint8_t n = (uint8_t)k;
    if (f(x, n) != x + 5 * (uint64_t)(n & 7)) return 1;
  }
  return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    SCOPED_TRACE(Level);
    auto Program = tmpFile("assume-oracle");
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
              Source.string(), Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    const auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}
} // namespace neverd::analysis::scalar_test

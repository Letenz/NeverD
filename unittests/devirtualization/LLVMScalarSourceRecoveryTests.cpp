//===- LLVMScalarSourceRecoveryTests.cpp - Whole-source preparation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarLoopRecoveryTest.h"

#include "neverd/analysis/LLVMScalarSourceRecovery.h"

namespace neverd::analysis::scalar_test {
namespace {
// Independently authored peeled arithmetic, with a full-width unused carrier.
// The carrier may disappear only after proving all of its original operations.
constexpr char NoisyPrefix[] = R"(
@unrelated = global i64 29
define i32 @f(i32 noundef %x, i8 noundef %n, i64 noundef %state) {
entry:
 %bound = and i8 %n, 3
 %empty = icmp eq i8 %bound, 0
 br i1 %empty, label %zero, label %peeled
zero:
 ret i32 %x
peeled:
 %first = add i32 %x, 23
 %one = icmp eq i8 %bound, 1
 br i1 %one, label %exit, label %pre
pre:
 br label %loop
loop:
 %index = phi i8 [1, %pre], [%next, %loop]
 %value = phi i32 [%first, %pre], [%sum, %loop]
 %unused = phi i64 [%state, %pre], [%unused.next, %loop]
 %unused.next = add i64 %unused, 13
 %sum = add i32 %value, 23
 %next = add nuw i8 %index, 1
 %done = icmp eq i8 %next, %bound
 br i1 %done, label %exit, label %loop
exit:
 %result = phi i32 [%first, %peeled], [%sum, %loop]
 ret i32 %result
}
)";

constexpr char FlagGuard[] = R"(
define i32 @f(i32 noundef %x, i32 noundef %n) {
  %small = and i32 %n, 7
  %before = add nsw i32 %small, -1
  %offset = add nuw i32 %small, 2147483646
  %combined = or i32 %before, %offset
  %condition = icmp sge i32 %combined, 0
  %other = add i32 %x, 11
  %result = select i1 %condition, i32 %x, i32 %other
  ret i32 %result
})";

constexpr char SplitGuard[] = R"(
define i32 @f(i32 noundef %x, i32 noundef %n) {
entry:
  %bound = and i32 %n, 7
  %shift = lshr i32 %n, 4
  %index = and i32 %shift, 7
  %bit = and i32 %n, 8
  %choose = icmp eq i32 %bit, 0
  br i1 %choose, label %left, label %right
left:
  %a = sub i32 %index, %bound
  %b = add i32 %a, -2147483647
  %c = and i32 %a, -2147483648
  %d = and i32 %c, %b
  %value.left = add i32 %x, 5
  br label %join
right:
  %e = sub i32 %index, %bound
  %f = add i32 %e, -2147483647
  %g = and i32 %e, -2147483648
  %h = and i32 %g, %f
  %value.right = xor i32 %x, 9
  br label %join
join:
  %flag = phi i32 [%d, %left], [%h, %right]
  %value = phi i32 [%value.left, %left], [%value.right, %right]
  %condition = icmp slt i32 %flag, 0
  br i1 %condition, label %yes, label %no
yes:
  ret i32 %value
no:
  %other = sub i32 %x, 3
  ret i32 %other
})";

LLVMScalarSourceRecoveryResult
prepare(Source &Input, const LLVMScalarSourceRecoveryLimits &Limits = {}) {
  const auto Before = Input.text();
  auto R = recoverLLVMScalarSource(Input.function(), Limits);
  EXPECT_EQ(Input.text(), Before);
  EXPECT_LE(R.ConstructionWork, Limits.Search.MaxConstructionWork);
  EXPECT_LE(R.ProofWork, Limits.Search.MaxProofWork);
  EXPECT_LE(R.Candidates, Limits.Search.MaxCandidates);
  EXPECT_LE(R.ProvedTransforms, Limits.Search.MaxTransforms);
  EXPECT_LE(R.CleanupRounds, Limits.MaxCleanupRounds);
  if (R.Module) {
    EXPECT_EQ(R.Status, RecoveryStatus::Recovered);
    EXPECT_FALSE(llvm::verifyModule(*R.Module));
    auto &F = *R.Module->getFunction("f");
    EXPECT_EQ(F.getFunctionType(), Input.function().getFunctionType());
    auto Proof = checkLLVMScalarEquivalence(Input.function(), F);
    EXPECT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
  } else {
    EXPECT_NE(R.Status, RecoveryStatus::Recovered);
  }
  return R;
}
} // namespace

TEST(LLVMScalarSourceRecovery, PreparationComposesWithRecoveredLoops) {
  Source Input(NoisyPrefix);
  auto R = prepare(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  EXPECT_GT(R.ProvedTransforms, 0U);
  EXPECT_GE(R.CleanupRounds, 2U);
  const auto Recovered = text(*R.Module);
  EXPECT_EQ(Recovered.find("%unused = phi"), std::string::npos);
  EXPECT_FALSE(R.Module->getGlobalVariable("unrelated"));
  auto &F = *R.Module->getFunction("f");
  EXPECT_EQ(F.getAttributes(), Input.function().getAttributes());
  EXPECT_EQ(F.arg_size(), 3U);
  EXPECT_TRUE(F.getArg(2)->use_empty());
}

TEST(LLVMScalarSourceRecovery, CleanupAloneMayProduceAProvedBody) {
  Source Input(R"(
define i32 @f(i32 noundef %x, i32 noundef %other) {
 %unused = add i32 %other, 31
 %zero = xor i32 %x, %x
 %result = add i32 %x, %zero
 ret i32 %result
}
)");
  auto R = prepare(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  EXPECT_EQ(R.ProvedTransforms, 0U);
  EXPECT_EQ(R.CleanupRounds, 1U);
  EXPECT_EQ(R.Module->getFunction("f")->getInstructionCount(), 1U);
}

TEST(LLVMScalarSourceRecovery, CleanupCannotEraseSourceObligations) {
  for (auto Operation :
       {"%bad = add nsw i64 %state, 13", "%bad = lshr exact i64 %state, 1",
        "%bad = udiv i64 17, %state",
        "%condition = icmp eq i64 %state, 0\n"
        " call void @llvm.assume(i1 %condition)"}) {
    SCOPED_TRACE(Operation);
    std::string IR = NoisyPrefix;
    replace(IR, " %bound =", std::string(" ") + Operation + "\n %bound =");
    IR += "\ndeclare void @llvm.assume(i1)\n";
    Source Input(IR);
    auto R = prepare(Input);
    EXPECT_FALSE(R.Module);
  }
}

TEST(LLVMScalarSourceRecovery, UnsupportedEffectsAreAdmittedBeforeCleanup) {
  std::string IR = NoisyPrefix;
  replace(IR, " %bound =", " %unused.call = call i64 @opaque()\n %bound =");
  IR += "\ndeclare i64 @opaque() readnone nounwind willreturn\n";
  Source Input(IR);
  auto R = prepare(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unsupported);
  EXPECT_FALSE(R.Module);
  EXPECT_EQ(R.CleanupRounds, 0U);
}

TEST(LLVMScalarSourceRecovery, ExactAndShortBudgetsRefuseAtomically) {
  Source Input(NoisyPrefix);
  auto Baseline = prepare(Input);
  ASSERT_TRUE(Baseline.Module) << Baseline.Diagnostic;
  for (bool Construction : {false, true}) {
    SCOPED_TRACE(Construction);
    auto Exact = Construction ? Baseline.ConstructionWork : Baseline.ProofWork;
    ASSERT_GT(Exact, 0U);
    for (unsigned Missing : {0U, 1U}) {
      LLVMScalarSourceRecoveryLimits Limits;
      (Construction ? Limits.Search.MaxConstructionWork
                    : Limits.Search.MaxProofWork) = Exact - Missing;
      auto R = prepare(Input, Limits);
      if (!Missing) {
        EXPECT_TRUE(R.Module) << R.Diagnostic;
      } else {
        EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded) << R.Diagnostic;
        EXPECT_FALSE(R.Module);
      }
    }
  }
}

TEST(LLVMScalarSourceRecovery, BoundaryAndContinuationLimitsCannotReset) {
  Source Input(NoisyPrefix);
  for (unsigned Kind = 0; Kind < 4; ++Kind) {
    SCOPED_TRACE(Kind);
    LLVMScalarSourceRecoveryLimits Limits;
    switch (Kind) {
    case 0:
      Limits.MaxBoundaryProofWork = 1;
      break;
    case 1:
      Limits.MaxCleanupRounds = 1;
      break;
    case 2:
      Limits.Search.MaxTransforms = 1;
      break;
    case 3:
      Limits.Search.Proof.MaxWork = 0;
      break;
    }
    auto R = prepare(Input, Limits);
    EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded) << R.Diagnostic;
    EXPECT_FALSE(R.Module);
  }
}

TEST(LLVMScalarSourceRecovery, ProvedFlagRemovalExposesModularGuard) {
  Source Input(FlagGuard);
  auto R = prepare(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  auto Output = text(*R.Module);
  EXPECT_EQ(Output.find("2147483646"), std::string::npos);
  EXPECT_NE(Output.find("icmp eq i32"), std::string::npos);
  EXPECT_NE(Input.text().find("add nsw"), std::string::npos);
  EXPECT_NE(Input.text().find("add nuw"), std::string::npos);
}

TEST(LLVMScalarSourceRecovery, HoistingExposesGuardAcrossDistinctPredecessors) {
  Source Input(SplitGuard);
  auto R = prepare(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  auto Output = text(*R.Module);
  EXPECT_EQ(Output.find("%flag = phi"), std::string::npos);
  EXPECT_EQ(Output.find("-2147483647"), std::string::npos);
  EXPECT_NE(Output.find("xor i32 %x, 9"), std::string::npos);
  EXPECT_NE(Output.find("add i32 %x, 5"), std::string::npos);
}

TEST(LLVMScalarSourceRecovery, RemovedFlagsStillRejectUndefinedOriginals) {
  for (const char *Operation :
       {"%bad = add nuw i32 %x, 1", "%bad = lshr exact i32 %x, 1",
        "%small.byte = trunc nuw i32 %x to i8",
        "%positive = zext nneg i32 %x to i64"}) {
    SCOPED_TRACE(Operation);
    std::string IR = FlagGuard;
    replace(IR, "  %small =", std::string(Operation) + "\n  %small =");
    Source Input(IR);
    EXPECT_FALSE(prepare(Input).Module);
  }
  std::string Overflow = FlagGuard;
  replace(Overflow, "add nuw i32 %small, 2147483646",
          "add nsw i32 %small, 2147483646");
  Source Input(Overflow);
  EXPECT_FALSE(prepare(Input).Module);
}

TEST(LLVMScalarSourceRecovery, GuardPreparationRetainsExactCumulativeLimits) {
  Source Input(FlagGuard);
  auto Base = prepare(Input);
  ASSERT_TRUE(Base.Module) << Base.Diagnostic;
  for (bool Construction : {false, true}) {
    LLVMScalarSourceRecoveryLimits Limits;
    auto &Budget = Construction ? Limits.Search.MaxConstructionWork
                                : Limits.Search.MaxProofWork;
    Budget = Construction ? Base.ConstructionWork : Base.ProofWork;
    EXPECT_TRUE(prepare(Input, Limits).Module);
    --Budget;
    auto Short = prepare(Input, Limits);
    EXPECT_EQ(Short.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(Short.Module);
  }
}

class LLVMScalarSourceCompiled : public NeverDLiftTest {};
TEST_F(LLVMScalarSourceCompiled, GuardProposalsMatchIndependentUnsignedOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (bool Split : {false, true}) {
    Source Input(Split ? SplitGuard : FlagGuard);
    auto R = prepare(Input);
    ASSERT_TRUE(R.Module) << R.Diagnostic;
    auto Harness = tmpFile("guard-oracle.c");
    std::ofstream(Harness)
        << R"(
#include <stdint.h>
extern uint32_t f(uint32_t, uint32_t);
int main(void) {
  uint32_t random = 0x196739u;
  for (unsigned low=0; low<256; ++low) {
    for (unsigned sample=0; sample<64; ++sample) {
      random = random*1664525u+1013904223u;
      uint32_t x = sample==0 ? 0 : sample==1 ? UINT32_MAX : random;
      uint32_t n = (random & ~255u) | low;
      uint32_t expected;
)"
        << (Split ? R"(
      uint32_t branch = (n&8u) ? x^9u : x+5u;
      expected = ((n>>4&7u)+1u)==(n&7u) ? branch : x-3u;
)"
                  : "      expected = (n&7u)==1u ? x : x+11u;\n")
        << R"(
      if (f(x,n)!=expected) return 1;
    }
  }
  return 0;
})";
    for (auto *M : {Input.Module.get(), R.Module.get()}) {
      auto IR = tmpFile("guard.ll");
      std::ofstream(IR) << text(*M);
      for (const char *Level : {"-O0", "-O2"}) {
        auto Program = tmpFile("guard-oracle");
        auto Built =
            exec(NEVERD_TEST_CLANG,
                 {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
                  IR.string(), Harness.string(), "-o", Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        auto Ran = exec(Program.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err;
      }
    }
  }
}

TEST_F(LLVMScalarSourceCompiled, OriginalAndPreparedMatchArithmeticOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  Source Input(NoisyPrefix);
  auto R = prepare(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  auto Harness = tmpFile("source-oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
extern uint32_t f(uint32_t, uint8_t, uint64_t);
int main(void) {
  uint64_t r = UINT64_MAX;
  for (unsigned n = 0; n < 256; ++n) {
    for (unsigned sample = 0; sample < 96; ++sample) {
      r = r * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
      uint32_t x = sample == 0 ? 0 : sample == 1 ? UINT32_MAX : (uint32_t)r;
      uint32_t expected = x + 23u * (n & 3u);
      if (f(x, (uint8_t)n, r) != expected) return 1;
    }
  }
  return 0;
}
)";
  for (auto *M : {Input.Module.get(), R.Module.get()}) {
    auto IR = tmpFile("source.ll");
    std::ofstream(IR) << text(*M);
    for (auto Level : {"-O0", "-O2"}) {
      auto Binary = tmpFile(std::string("source-oracle") +
                            neverd::test::executableSuffix());
      auto Built =
          exec(NEVERD_TEST_CLANG,
               {Level, IR.string(), Harness.string(), "-o", Binary.string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
      auto Ran = exec(Binary.string(), {});
      EXPECT_TRUE(Ran.ok()) << Ran.err;
    }
  }
}
} // namespace neverd::analysis::scalar_test

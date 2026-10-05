//===- LLVMScalarLoopBoundTests.cpp - Proved scalar exit bounds -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarLoopRecoveryTest.h"

namespace neverd::analysis::scalar_test {
namespace {
// Independent arithmetic loops. The sign expression means delta == -1 only
// on the actual bounded loop domain; it is not an unrestricted identity.
std::string encoded(unsigned Width = 32, bool Descending = false,
                    bool Inverted = false, bool ZeroTrip = false) {
  const auto Type = "i" + std::to_string(Width);
  const auto Sign = UINT64_C(1) << (Width - 1);
  std::string IR = R"(
@unrelated = global i32 29
define i32 @f(i32 noundef %x, $T noundef %n) {
entry:
 %bits = and $T %n, 7
 %bound = add $T %bits, 1
 br label %head
head:
 %i = phi $T [0, %entry], [%next, %latch]
 %v = phi i32 [%x, %entry], [%sum, %latch]
 %sum = add i32 %v, 13
 %delta = sub $T %i, %bound
 %biased = add $T %delta, $BIAS
 %sign = and $T %delta, $SIGN
 %flags = and $T %sign, %biased
 %more = icmp eq $T %flags, 0
 br i1 %more, label %latch, label %exit
latch:
 %next = add $T %i, 1
 br label %head
exit:
 ret i32 %sum
})";
  if (Descending) {
    replace(IR, "add $T %bits, 1", "add $T %bits, 0");
    replace(IR, "[0, %entry]", "[8, %entry]");
    replace(IR, "sub $T %i, %bound", "sub $T %bound, %i");
    replace(IR, "add $T %i, 1", "add $T %i, -1");
  }
  if (Inverted) {
    replace(IR, "icmp eq $T %flags, 0", "icmp ne $T %flags, 0");
    replace(IR, "br i1 %more, label %latch, label %exit",
            "br i1 %more, label %exit, label %latch");
  }
  if (ZeroTrip) {
    replace(IR, "add $T %bits, 1", "add $T %bits, 0");
    replace(IR, " br label %head\nhead:",
            " %empty = icmp eq $T %bound, 0\n"
            " br i1 %empty, label %zero, label %head\n"
            "zero:\n ret i32 %x\nhead:");
  }
  for (auto Pair :
       {std::pair{"$T", Type}, std::pair{"$SIGN", std::to_string(Sign)},
        std::pair{"$BIAS", std::to_string(Sign + 1)}}) {
    for (size_t At = 0; (At = IR.find(Pair.first, At)) != std::string::npos;) {
      IR.replace(At, std::char_traits<char>::length(Pair.first), Pair.second);
      At += Pair.second.size();
    }
  }
  return IR;
}

void expectComparison(Source &Input) {
  auto R = recover(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  auto Output = text(*R.Module);
  EXPECT_EQ(Output.find("%flags ="), std::string::npos);
  auto &F = *R.Module->getFunction("f");
  EXPECT_EQ(F.getFunctionType(), Input.function().getFunctionType());
  EXPECT_EQ(F.getAttributes(), Input.function().getAttributes());
  EXPECT_FALSE(R.Module->getGlobalVariable("unrelated"));
}
} // namespace

TEST(LLVMScalarLoopBounds, ProvesModularGuardsAcrossIntegerWidths) {
  for (unsigned Width : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Width);
    Source Input(encoded(Width));
    expectComparison(Input);
  }
}

TEST(LLVMScalarLoopBounds, DescendingPolarityAndZeroTripRemainProposals) {
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    SCOPED_TRACE(Kind);
    Source Input(encoded(32, Kind == 0, Kind == 1, Kind == 2));
    expectComparison(Input);
  }
}

TEST(LLVMScalarLoopBounds, SharedConditionRetainsBothExitingEdges) {
  auto IR = encoded();
  replace(IR, " %bound =",
          " %choice = and i32 %n, 8\n"
          " %choose = icmp eq i32 %choice, 0\n %bound =");
  replace(IR, " br i1 %more, label %latch, label %exit",
          " br i1 %choose, label %left, label %right\n"
          "left:\n br i1 %more, label %latch, label %exit\n"
          "right:\n br i1 %more, label %latch, label %exit");
  Source Input(IR);
  expectComparison(Input);
}

TEST(LLVMScalarLoopBounds, CompleteDataProofRejectsNeighborGuesses) {
  auto IR = encoded();
  replace(IR, "and i32 %n, 7", "and i32 %n, 3");
  replace(IR, "add i32 %bits, 1", "add i32 %bits, 3");
  replace(IR, "add i32 %v, 13", "add i32 %v, %x");
  replace(IR, "%delta = sub i32 %i, %bound",
          "%late = add i32 %bound, 2\n %delta = sub i32 %i, %late");
  replace(IR, "icmp eq i32 %flags, 0", "icmp slt i32 %delta, 0");
  // The zero-data executions agree at every proposed earlier boundary. All
  // remaining x bits must still be symbolic in the mandatory full proof.
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
  EXPECT_GT(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopBounds, CounterStepDoesNotEstablishDivisibility) {
  auto IR = encoded();
  replace(IR, "and i32 %n, 7", "and i32 %n, 3");
  replace(IR, "%bound = add i32 %bits, 1",
          "%even = mul i32 %bits, 2\n %bound = add i32 %even, 1");
  replace(IR, "add i32 %i, 1", "add i32 %i, 2");
  Source Input(IR);
  LLVMScalarLoopRecoveryLimits Limits;
  Limits.Proof.MaxBlockVisits = 64;
  auto R = recover(Input, Limits);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
  EXPECT_GT(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopBounds, OversizedDependencySlicesSupplyNoBoundary) {
  auto IR = encoded();
  std::string Chain;
  std::string Previous = "%flags";
  for (unsigned I = 0; I < 40; ++I) {
    auto Next = "%copy" + std::to_string(I);
    Chain += Next + " = xor i32 " + Previous + ", 0\n ";
    Previous = Next;
  }
  replace(IR, "%more = icmp eq i32 %flags, 0",
          Chain + "%more = icmp eq i32 " + Previous + ", 0");
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopBounds, OriginalPoisonAndMissingBoundsRemainRefused) {
  auto IR = encoded();
  replace(IR, " %bound =", " %dead = add nuw i32 %bits, 4294967295\n %bound =");
  Source Poison(IR);
  auto R = recover(Poison);
  EXPECT_EQ(R.Status, RecoveryStatus::Unsupported);
  EXPECT_EQ(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
  IR = encoded();
  // The masked bound now lives inside the loop. Its wider input is not an
  // available same-width boundary, and discovery must not invent a cast.
  replace(IR, "i32 noundef %n", "i64 noundef %n");
  replace(IR, " %bits = and i32 %n, 7\n %bound = add i32 %bits, 1\n", "");
  replace(IR, " %sum =",
          " %small = trunc i64 %n to i32\n"
          " %bits = and i32 %small, 7\n"
          " %bound = add i32 %bits, 1\n %sum =");
  Source Missing(IR);
  R = recover(Missing);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopBounds, CumulativeLimitsPublishNoPartialBody) {
  Source Input(encoded());
  auto Base = recover(Input);
  ASSERT_TRUE(Base.Module) << Base.Diagnostic;
  for (bool Proof : {false, true}) {
    LLVMScalarLoopRecoveryLimits Limits;
    auto &Budget = Proof ? Limits.MaxProofWork : Limits.MaxConstructionWork;
    Budget = Proof ? Base.ProofWork : Base.ConstructionWork;
    EXPECT_TRUE(recover(Input, Limits).Module);
    --Budget;
    auto Short = recover(Input, Limits);
    EXPECT_EQ(Short.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(Short.Module);
  }
  for (bool Transform : {false, true}) {
    LLVMScalarLoopRecoveryLimits Limits;
    if (Transform)
      Limits.MaxTransforms = 1;
    else
      Limits.MaxCandidates = 1;
    auto Short = recover(Input, Limits);
    EXPECT_EQ(Short.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(Short.Module);
  }
}

TEST(LLVMScalarLoopBounds, UnusedArgumentsCannotDisplaceTheLivePrefixSeed) {
  std::string IR = "define i32 @f(";
  for (unsigned I = 0; I < 40; ++I)
    IR += "i32 noundef %unused" + std::to_string(I) + ", ";
  IR += R"(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 7
 %empty = icmp eq i8 %bound, 0
 br i1 %empty, label %zero, label %peeled
zero: ret i32 %x
peeled:
 %first = add i32 %x, 13
 %one = icmp eq i8 %bound, 1
 br i1 %one, label %exit, label %pre
pre: br label %head
head:
 %i = phi i8 [1, %pre], [%next, %head]
 %v = phi i32 [%first, %pre], [%sum, %head]
 %sum = add i32 %v, 13
 %next = add i8 %i, 1
 %done = icmp eq i8 %next, %bound
 br i1 %done, label %exit, label %head
exit:
 %result = phi i32 [%first, %peeled], [%sum, %head]
 ret i32 %result
})";
  Source Input(IR);
  LLVMScalarLoopRecoveryLimits Limits;
  Limits.MaxCandidates = 16;
  auto R = recover(Input, Limits);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  EXPECT_EQ(R.Module->getFunction("f")->arg_size(), 42U);
  EXPECT_EQ(text(*R.Module).find("peeled:"), std::string::npos);
}

class LLVMScalarLoopBoundsCompiled : public NeverDLiftTest {};
TEST_F(LLVMScalarLoopBoundsCompiled, OriginalAndRecoveredMatchUnsignedOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    SCOPED_TRACE(Kind);
    unsigned Width = Kind < 4 ? (8U << Kind) : 32U;
    Source Input(encoded(Width, Kind == 4, Kind == 5, Kind == 6));
    auto R = recover(Input);
    ASSERT_TRUE(R.Module) << R.Diagnostic;
    auto Harness = tmpFile("bound-oracle.c");
    std::ofstream(Harness)
        << "#include <stdint.h>\nextern uint32_t f(uint32_t, uint" << Width
        << "_t);\n"
        << R"(
int main(void) {
  uint64_t r = UINT64_C(0x15379bdf2468ace0);
  for (unsigned low=0; low<256; ++low) {
    for (unsigned sample=0; sample<64; ++sample) {
      r = r*UINT64_C(6364136223846793005)+UINT64_C(1442695040888963407);
      uint32_t x = sample==0 ? 0 : sample==1 ? UINT32_MAX : (uint32_t)r;
      uint64_t n = (r & ~UINT64_C(255)) | low;
      uint32_t count = )"
        << (Kind == 4   ? "8u-(n&7u)"
            : Kind == 6 ? "n&7u"
                        : "(n&7u)+1u")
        << ";\n      if (f(x,n)!=x+13u*count) return 1;\n"
        << "    }\n  }\n  return 0;\n}\n";
    for (auto *M : {Input.Module.get(), R.Module.get()}) {
      auto IR = tmpFile("bound.ll");
      std::ofstream(IR) << text(*M);
      for (const char *Level : {"-O0", "-O2"}) {
        auto Program = tmpFile(std::string("bound-oracle") +
                               neverd::test::executableSuffix());
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
} // namespace neverd::analysis::scalar_test

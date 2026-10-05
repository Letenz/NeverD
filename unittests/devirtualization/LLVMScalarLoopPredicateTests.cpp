//===- LLVMScalarLoopPredicateTests.cpp - Loop condition proposals --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarLoopRecoveryTest.h"

namespace neverd::analysis::scalar_test {
namespace {
// Independently authored loops: the Boolean carrier repeats the integer
// comparison, but that relationship must be proved across all actual edges.
constexpr char Predicate[] = R"(
@unrelated = global i32 17
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 7
 %begin = icmp ne i8 0, %bound
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %go = phi i1 [%begin, %entry], [%more, %body]
 br i1 %go, label %body, label %exit
body:
 %sum = add i32 %v, 9
 %next = add i8 %i, 1
 %more = icmp ne i8 %next, %bound
 br label %head
exit:
 ret i32 %v
})";

constexpr char MultipleEntries[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bits = and i8 %n, 7
 %bound = add i8 %bits, 1
 %bit = and i8 %n, 8
 %choose = icmp eq i8 %bit, 0
 br i1 %choose, label %left, label %right
left:
 %begin.left = icmp ne i8 0, %bound
 br label %head
right:
 %begin.right = icmp ne i8 1, %bound
 br label %head
head:
 %i = phi i8 [0, %left], [1, %right], [%next, %body]
 %v = phi i32 [%x, %left], [%x, %right], [%sum, %body]
 %go = phi i1 [%begin.left, %left], [%begin.right, %right], [%more, %body]
 br i1 %go, label %body, label %exit
body:
 %sum = add i32 %v, 9
 %next = add i8 %i, 1
 %more = icmp ne i8 %next, %bound
 br label %head
exit:
 ret i32 %v
})";

std::string sameWidth(unsigned Width) {
  std::string IR = Predicate;
  for (size_t At = 0; (At = IR.find("i8", At)) != std::string::npos;) {
    auto Type = "i" + std::to_string(Width);
    IR.replace(At, 2, Type);
    At += Type.size();
  }
  return IR;
}

std::string widened(bool Signed) {
  std::string IR = Predicate;
  replace(IR, " %begin =",
          Signed ? " %wide = sext i8 %bound to i32\n %begin ="
                 : " %wide = zext i8 %bound to i32\n %begin =");
  replace(IR, "phi i8 [0", "phi i32 [0");
  replace(IR, "add i8 %i", "add i32 %i");
  replace(IR, "icmp ne i8 %next, %bound", "icmp ne i32 %next, %wide");
  if (Signed) {
    replace(IR, "%bound = and i8 %n, 7",
            "%bits = and i8 %n, 3\n %bound = or i8 %bits, -4");
    replace(IR, "icmp ne i8 0, %bound", "icmp ne i8 -4, %bound");
    replace(IR, "phi i32 [0, %entry]", "phi i32 [-4, %entry]");
  }
  return IR;
}

void expectRetained(Source &Input) {
  auto R = recover(Input);
  EXPECT_TRUE(R.Status == RecoveryStatus::Unchanged ||
              R.Status == RecoveryStatus::Recovered)
      << R.Diagnostic;
  auto Output = R.Module ? text(*R.Module) : Input.text();
  EXPECT_NE(Output.find("%go = phi i1"), std::string::npos);
}
} // namespace

TEST(LLVMScalarLoopPredicates, RecoversComparisonsAcrossIntegerWidths) {
  for (unsigned Width : {8U, 16U, 32U, 64U}) {
    SCOPED_TRACE(Width);
    Source Input(sameWidth(Width));
    auto R = recover(Input);
    ASSERT_TRUE(R.Module) << R.Diagnostic;
    EXPECT_EQ(text(*R.Module).find("%go = phi"), std::string::npos);
    auto &F = *R.Module->getFunction("f");
    EXPECT_EQ(F.getFunctionType(), Input.function().getFunctionType());
    EXPECT_EQ(F.getAttributes(), Input.function().getAttributes());
    EXPECT_FALSE(R.Module->getGlobalVariable("unrelated"));
  }
}

TEST(LLVMScalarLoopPredicates, SwapsOrderingAndPreservesBranchPolarity) {
  for (bool Inverted : {false, true}) {
    std::string IR = Predicate;
    replace(IR, "icmp ne i8 0, %bound",
            Inverted ? "icmp ule i8 %bound, 0" : "icmp ugt i8 %bound, 0");
    replace(IR, "icmp ne i8 %next, %bound",
            Inverted ? "icmp uge i8 %next, %bound"
                     : "icmp ult i8 %next, %bound");
    if (Inverted)
      replace(IR, "br i1 %go, label %body, label %exit",
              "br i1 %go, label %exit, label %body");
    Source Input(IR);
    auto R = recover(Input);
    ASSERT_TRUE(R.Module) << R.Diagnostic;
    EXPECT_EQ(text(*R.Module).find("%go = phi"), std::string::npos);
  }
}

TEST(LLVMScalarLoopPredicates, ZeroAndSignExtendedBoundsRequireMatchingSeeds) {
  for (bool Signed : {false, true}) {
    SCOPED_TRACE(Signed);
    Source Input(widened(Signed));
    auto R = recover(Input);
    ASSERT_TRUE(R.Module) << R.Diagnostic;
    auto Output = text(*R.Module);
    EXPECT_EQ(Output.find("%go = phi"), std::string::npos);
    if (Signed)
      EXPECT_NE(Output.find("sext i8 %bound to i32"), std::string::npos);
  }
}

TEST(LLVMScalarLoopPredicates, ChecksEveryExternalEdgeWithDistinctSeeds) {
  Source Input(MultipleEntries);
  auto R = recover(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%go = phi"), std::string::npos);
  for (unsigned Kind = 0; Kind < 3; ++Kind) {
    SCOPED_TRACE(Kind);
    std::string IR = MultipleEntries;
    if (Kind == 0) {
      replace(IR, " %bit =", " %other = xor i8 %bound, 0\n %bit =");
      replace(IR, "icmp ne i8 1, %bound", "icmp ne i8 1, %other");
    } else if (Kind == 1) {
      replace(IR, "icmp ne i8 1, %bound", "icmp eq i8 1, %bound");
    } else {
      replace(IR, "phi i8 [0, %left], [1, %right]",
              "phi i8 [0, %left], [0, %right]");
    }
    Source Other(IR);
    expectRetained(Other);
  }
}

TEST(LLVMScalarLoopPredicates, EntryAgreementCannotAssumeTheBackedge) {
  std::string IR = Predicate;
  replace(IR, " %begin =", " %late = add i8 %bound, 1\n %begin =");
  replace(IR, "icmp ne i8 %next, %bound", "icmp ne i8 %next, %late");
  // Zero-data screening cannot distinguish an extra addition of x. The full
  // symbolic-data proof must still reject the guessed trip count.
  replace(IR, "add i32 %v, 9", "add i32 %v, %x");
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_GT(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
}

TEST(LLVMScalarLoopPredicates, NoImplicitTruncationOfTheEntryBound) {
  std::string IR = Predicate;
  replace(IR, "i8 noundef %n", "i16 noundef %n");
  replace(IR, "and i8 %n", "and i16 %n");
  replace(IR, "icmp ne i8 0, %bound", "icmp ne i16 0, %bound");
  replace(IR, "%more = icmp ne i8 %next, %bound",
          "%wide.next = zext i8 %next to i16\n"
          " %more = icmp ne i16 %wide.next, %bound");
  Source Input(IR);
  expectRetained(Input);
}

TEST(LLVMScalarLoopPredicates, EveryBackedgeMustPreserveTheComparison) {
  std::string IR = Predicate;
  replace(IR, " %begin =", " %late = add i8 %bound, 1\n %begin =");
  replace(IR, "[%next, %body]", "[%next, %left], [%next, %right]");
  replace(IR, "[%sum, %body]", "[%sum, %left], [%sum, %right]");
  replace(IR, "[%more, %body]", "[%more, %left], [%more.right, %right]");
  replace(IR, " %more = icmp ne i8 %next, %bound\n br label %head",
          " %bit = and i8 %i, 1\n"
          " %choose = icmp eq i8 %bit, 0\n"
          " br i1 %choose, label %left, label %right\n"
          "left:\n %more = icmp ne i8 %next, %bound\n br label %head\n"
          "right:\n %more.right = icmp ult i8 %next, %bound\n br label %head");
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_TRUE(R.Module) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%go = phi"), std::string::npos);
  replace(IR, "icmp ult i8 %next, %bound", "icmp ult i8 %next, %late");
  Source Other(IR);
  expectRetained(Other);
}

TEST(LLVMScalarLoopPredicates, OriginalPoisonAndNonterminationCannotDisappear) {
  for (bool Nontermination : {false, true}) {
    std::string IR = Predicate;
    if (Nontermination)
      replace(IR, "add i8 %i, 1", "add i8 %i, 0");
    else
      replace(IR, " %bound =", " %dead = add nuw i32 %x, 1\n %bound =");
    Source Input(IR);
    auto R = recover(Input);
    EXPECT_FALSE(R.Module);
    EXPECT_EQ(R.Candidates, 0U);
    EXPECT_FALSE(R.Diagnostic.empty());
  }
}

TEST(LLVMScalarLoopPredicates, ConstructionProofAndCandidateLimitsAreAtomic) {
  Source Input(widened(true));
  auto Base = recover(Input);
  ASSERT_TRUE(Base.Module) << Base.Diagnostic;
  for (bool Proof : {false, true}) {
    LLVMScalarLoopRecoveryLimits L;
    auto &Budget = Proof ? L.MaxProofWork : L.MaxConstructionWork;
    Budget = Proof ? Base.ProofWork : Base.ConstructionWork;
    EXPECT_TRUE(recover(Input, L).Module);
    --Budget;
    auto R = recover(Input, L);
    EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(R.Module);
  }
  for (bool Transform : {false, true}) {
    LLVMScalarLoopRecoveryLimits L;
    if (Transform)
      L.MaxTransforms = 1;
    else
      L.MaxCandidates = 1;
    auto R = recover(Input, L);
    EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(R.Module);
  }
}

class LLVMScalarLoopPredicatesCompiled : public NeverDLiftTest {};
TEST_F(LLVMScalarLoopPredicatesCompiled,
       OriginalAndRecoveredMatchUnsignedOracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    SCOPED_TRACE(Kind);
    unsigned Width = Kind < 4 ? (8U << Kind) : 8U;
    Source Input(Kind < 4    ? sameWidth(Width)
                 : Kind == 6 ? MultipleEntries
                             : widened(Kind == 5));
    auto R = recover(Input);
    ASSERT_TRUE(R.Module) << R.Diagnostic;
    auto Harness = tmpFile("predicate-oracle.c");
    std::ofstream(Harness)
        << "#include <stdint.h>\nextern uint32_t f(uint32_t, uint" << Width
        << "_t);\n"
        << R"(
int main(void) {
  uint64_t r = UINT64_C(0x876543218badf00d);
  for (unsigned low=0; low<256; ++low) {
    for (unsigned sample=0; sample<64; ++sample) {
      r = r*UINT64_C(6364136223846793005)+UINT64_C(1442695040888963407);
      uint32_t x = sample==0 ? 0 : sample==1 ? UINT32_MAX : (uint32_t)r;
      uint64_t n = (r & ~UINT64_C(255)) | low;
      uint32_t count = )"
        << (Kind == 5   ? "n&3u"
            : Kind == 6 ? "(n&7u)+1u-((n>>3)&1u)"
                        : "n&7u")
        << ";\n      if (f(x,n)!=x+9u*count) return 1;\n"
        << "    }\n  }\n  return 0;\n}\n";
    for (auto *M : {Input.Module.get(), R.Module.get()}) {
      auto IR = tmpFile("predicate.ll");
      std::ofstream(IR) << text(*M);
      for (const char *Level : {"-O0", "-O2"}) {
        auto Program = tmpFile(std::string("predicate-oracle") +
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

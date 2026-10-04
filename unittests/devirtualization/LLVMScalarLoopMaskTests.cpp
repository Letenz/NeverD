//===- LLVMScalarLoopMaskTests.cpp - Carrier mask candidates -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarLoopRecoveryTest.h"

namespace neverd::analysis::scalar_test {
namespace {
constexpr char MaskedCounter[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %masked = and i8 %i, 15
 %more = icmp ult i8 %masked, %bound
 br i1 %more, label %body, label %exit
body:
 %sum = add i32 %v, 9
 %next = add nuw i8 %masked, 1
 br label %head
exit: ret i32 %v
})";

constexpr char DataMask[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %masked = and i32 %v, 255
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %sum = add i32 %masked, 9
 %next = add nuw i8 %i, 1
 br label %head
exit: ret i32 %v
})";
} // namespace

TEST(LLVMScalarLoopMasks, IdentityMaskExposesTheActualCounter) {
  for (const char *Triple : {"x86_64-linux-gnu", "aarch64-linux-gnu",
                             "aarch64_be-linux-gnu", "armv7-linux-gnueabihf"}) {
    SCOPED_TRACE(Triple);
    for (bool Commute : {false, true}) {
      std::string IR =
          std::string("target triple = \"") + Triple + "\"\n" + MaskedCounter;
      if (Commute)
        replace(IR, "and i8 %i, 15", "and i8 15, %i");
      Source Input(IR);
      auto R = recover(Input);
      ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
      EXPECT_EQ(text(*R.Module).find("%masked"), std::string::npos);
      EXPECT_NE(text(*R.Module).find("add nuw i8 %i, 1"), std::string::npos);
    }
  }
}

TEST(LLVMScalarLoopMasks, ZeroFieldMayReplaceOnlyItsObservedBits) {
  std::string IR = DataMask;
  replace(IR, "br label %head", "%seed = and i32 %x, 255\n br label %head");
  replace(IR, "[%x, %entry], [%sum, %body]",
          "[%seed, %entry], [%wrapped, %body]");
  replace(IR, "%masked = and i32 %v, 255", "%masked = and i32 %v, -256");
  replace(IR, "%sum = add i32 %masked, 9",
          "%sum = add i32 %v, 3\n %wrapped = and i32 %sum, 255");
  replace(IR, "ret i32 %v",
          "%observed = add i32 %v, %masked\n ret i32 %observed");
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%masked"), std::string::npos);
  EXPECT_NE(text(*R.Module).find("%v = phi"), std::string::npos);
}

TEST(LLVMScalarLoopMasks, FullDataFailureTriesTheScreenedAlternative) {
  std::string IR = DataMask;
  replace(IR, "br label %head", "%delta = and i32 %x, 15\n br label %head");
  replace(IR, "[%x, %entry], [%sum, %body]", "[0, %entry], [%sum, %body]");
  replace(IR, "%masked = and i32 %v, 255", "%masked = and i32 %v, 15");
  replace(IR, "%sum = add i32 %masked, 9", "%sum = xor i32 %masked, %delta");
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto Output = text(*R.Module);
  EXPECT_EQ(Output.find("%masked"), std::string::npos);
  EXPECT_NE(Output.find("xor i32 %v, %delta"), std::string::npos);
  // Zero input data makes both proposals look correct. The full-data query
  // must refuse zero before accepting the actual evolving value.
  EXPECT_NE(Output.find("%v = phi"), std::string::npos);
}

TEST(LLVMScalarLoopMasks, HighInputBitsCannotBeDroppedByTheScreen) {
  for (bool High : {false, true}) {
    std::string IR = DataMask;
    if (High) {
      replace(IR, "%masked = and i32 %v, 255", "%masked = and i32 %v, -256");
      replace(IR, "%sum = add i32 %masked, 9", "%sum = add i32 %v, 9");
      replace(IR, "ret i32 %v", "ret i32 %masked");
    }
    Source Input(IR);
    auto R = recover(Input);
    EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
    EXPECT_GT(R.Candidates, 0U);
    EXPECT_FALSE(R.Module);
  }
}

TEST(LLVMScalarLoopMasks, WrapAndNewOverflowKeepTheMask) {
  for (bool Poison : {false, true}) {
    std::string IR = DataMask;
    replace(IR, "[%x, %entry], [%sum, %body]",
            Poison ? "[255, %entry], [%sum, %body]"
                   : "[14, %entry], [%sum, %body]");
    replace(IR, "%masked = and i32 %v, 255",
            Poison ? "%masked = and i8 %v, 127" : "%masked = and i32 %v, 15");
    replace(IR, "%sum = add i32 %masked, 9",
            Poison ? "%sum = add nuw i8 %masked, 1"
                   : "%sum = add i32 %masked, 1");
    if (Poison) {
      replace(IR, "%v = phi i32", "%v = phi i8");
      replace(IR, "ret i32 %v",
              "%result = zext i8 %v to i32\n ret i32 %result");
    }
    Source Input(IR);
    auto R = recover(Input);
    EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
    EXPECT_GT(R.Candidates, 0U);
    EXPECT_FALSE(R.Module);
  }
}

TEST(LLVMScalarLoopMasks, ExactAndShortBudgetsPreserveAtomicRefusal) {
  Source Input(MaskedCounter);
  auto Base = recover(Input);
  ASSERT_EQ(Base.Status, RecoveryStatus::Recovered) << Base.Diagnostic;
  for (bool Proof : {false, true}) {
    LLVMScalarLoopRecoveryLimits L;
    auto &Limit = Proof ? L.MaxProofWork : L.MaxConstructionWork;
    Limit = Proof ? Base.ProofWork : Base.ConstructionWork;
    EXPECT_EQ(recover(Input, L).Status, RecoveryStatus::Recovered);
    --Limit;
    auto Short = recover(Input, L);
    EXPECT_EQ(Short.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(Short.Module);
  }
  LLVMScalarLoopRecoveryLimits L;
  L.MaxCandidates = Base.Candidates - 1;
  auto Short = recover(Input, L);
  EXPECT_EQ(Short.Status, RecoveryStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Module);
}

TEST(LLVMScalarLoopMasks, EveryBackedgeContributesToTheMaskObligation) {
  constexpr char IR[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %a], [%next, %b]
 %v = phi i32 [1, %entry], [%left, %a], [%right, %b]
 %masked = and i32 %v, 15
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %next = add nuw i8 %i, 1
 %parity = and i8 %i, 1
 %even = icmp eq i8 %parity, 0
 br i1 %even, label %a, label %b
a:
 %left = add i32 %masked, 1
 br label %head
b:
 %right = add i32 %masked, 2
 br label %head
exit:
 %result = add i32 %v, %x
 ret i32 %result
})";
  Source Good(IR);
  auto R = recover(Good);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%masked"), std::string::npos);
  std::string Wrong = IR;
  replace(Wrong, "add i32 %masked, 2", "add i32 %masked, 256");
  Source Bad(Wrong);
  R = recover(Bad);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged) << R.Diagnostic;
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopMasks, MoreThanOneBatchKeepsEveryObservedTerm) {
  std::string IR = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
)";
  std::string Previous = "%v";
  for (unsigned N = 0; N < 35; ++N) {
    const auto Suffix = std::to_string(N);
    IR += "%mask" + Suffix + " = and i8 %i, 15\n";
    IR += "%wide" + Suffix + " = zext i8 %mask" + Suffix + " to i32\n";
    IR +=
        "%term" + Suffix + " = add i32 " + Previous + ", %wide" + Suffix + "\n";
    Previous = "%term" + Suffix;
  }
  IR += "%sum = add i32 " + Previous + ", 9\n";
  IR += "%next = add nuw i8 %i, 1\n br label %head\nexit: ret i32 %v\n}";
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_GE(R.ProvedTransforms, 2U);
  EXPECT_EQ(text(*R.Module).find("%mask"), std::string::npos);
  // A separately written closed form checks that batching retained all 35
  // contributions, including zero- and single-trip cases.
  constexpr char Closed[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
 %low = and i8 %n, 3
 %k = zext i8 %low to i32
 %last = sub i32 %k, 1
 %product = mul i32 %k, %last
 %pairs = lshr i32 %product, 1
 %triangular = mul i32 %pairs, 35
 %steps = mul i32 %k, 9
 %delta = add i32 %triangular, %steps
 %result = add i32 %x, %delta
 ret i32 %result
})";
  EXPECT_EQ(check(text(*R.Module), Closed).Status, Status::Proved);
}

TEST(LLVMScalarLoopMasks, SymbolicXorBackedgesUseSharedMaskFacts) {
  constexpr char IR[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %delta = and i32 %x, 15
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %a], [%next, %b]
 %v = phi i32 [0, %entry], [%left, %a], [%right, %b]
 %masked = and i32 %v, 15
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %next = add nuw i8 %i, 1
 %parity = and i8 %i, 1
 %even = icmp eq i8 %parity, 0
 br i1 %even, label %a, label %b
a:
 %left = xor i32 %masked, %delta
 br label %head
b:
 %right = xor i32 %masked, 7
 br label %head
exit: ret i32 %v
})";
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%masked"), std::string::npos);
  constexpr char Closed[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
 %k = and i8 %n, 3
 %delta = and i32 %x, 15
 %odd = icmp eq i8 %k, 1
 %last = icmp eq i8 %k, 3
 %empty = icmp eq i8 %k, 0
 %second = xor i32 %delta, 7
 %a = select i1 %odd, i32 %delta, i32 %second
 %b = select i1 %last, i32 7, i32 %a
 %result = select i1 %empty, i32 0, i32 %b
 ret i32 %result
})";
  EXPECT_EQ(check(IR, Closed).Status, Status::Proved);
  EXPECT_EQ(check(text(*R.Module), Closed).Status, Status::Proved);
}
} // namespace neverd::analysis::scalar_test

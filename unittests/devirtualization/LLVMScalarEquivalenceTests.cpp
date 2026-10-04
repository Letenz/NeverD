//===- LLVMScalarEquivalenceTests.cpp - Complete scalar partition proofs --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarEquivalenceTest.h"

namespace neverd::analysis::scalar_test {
namespace {
constexpr char Counted[] = R"(
define i32 @f(i32 noundef %x, i32 noundef %n) {
entry:
  %limit = and i32 %n, 7
  br label %head
head:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = phi i32 [%x, %entry], [%sum, %body]
  %go = icmp ult i32 %i, %limit
  br i1 %go, label %body, label %exit
body:
  %sum = add i32 %v, 9
  %next = add nuw nsw i32 %i, 1
  br label %head
exit:
  ret i32 %v
})";
constexpr char Closed[] = R"(
define i32 @f(i32 noundef %x, i32 noundef %n) {
  %limit = and i32 %n, 7
  %delta = mul nuw nsw i32 %limit, 9
  %v = add i32 %delta, %x
  ret i32 %v
})";
} // namespace

TEST(LLVMScalarEquivalence, CompleteLoopDomainRetainsAllDataBits) {
  auto R = check(Counted, Closed);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 8U);
  EXPECT_EQ(R.ControlBits,
            (std::vector<LLVMScalarControlBit>{{1, 0}, {1, 1}, {1, 2}}));
  EXPECT_GT(R.Attempts, R.CompletedPartitions);
  // High data bits and high bits of the control word are not zeroed.
  for (const char *Mask : {"%x", "%n"}) {
    std::string Bad = Closed;
    const auto At = Bad.find("ret i32 %v");
    Bad.replace(At, 10,
                std::string("%high = and i32 ") + Mask +
                    ", -2147483648\n%wrong = xor i32 %v, %high\n"
                    "ret i32 %wrong");
    EXPECT_EQ(check(Counted, Bad).Status, Status::Unproved);
  }
}

TEST(LLVMScalarEquivalence, LastPartitionCounterexampleCannotBeSkipped) {
  std::string Bad = Closed;
  Bad.replace(Bad.find("ret i32 %v"), 10,
              "%last = icmp eq i32 %limit, 7\n"
              "%bad = add i32 %v, 1\n"
              "%answer = select i1 %last, i32 %bad, i32 %v\n"
              "ret i32 %answer");
  auto R = check(Counted, Bad);
  EXPECT_EQ(R.Status, Status::Unproved);
  EXPECT_EQ(R.CompletedPartitions, 7U);
}

TEST(LLVMScalarEquivalence, BothFunctionsContributeControlBits) {
  constexpr char A[] = R"(
define i16 @f(i16 noundef %x, i8 noundef %n) {
 %bit = and i8 %n, 1
 %c = icmp eq i8 %bit, 0
 br i1 %c, label %a, label %b
a: ret i16 %x
b: ret i16 %x
})";
  constexpr char B[] = R"(
define i16 @f(i16 noundef %x, i8 noundef %n) {
 %bit = and i8 %n, 2
 %c = icmp eq i8 %bit, 0
 br i1 %c, label %a, label %b
a: ret i16 %x
b: ret i16 %x
})";
  auto R = check(A, B);
  ASSERT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 4U);
  EXPECT_EQ(R.ControlBits, (std::vector<LLVMScalarControlBit>{{1, 0}, {1, 1}}));
}

TEST(LLVMScalarEquivalence, ParallelPhiCopiesPreserveLoopSwaps) {
  constexpr char Loop[] = R"(
define i64 @f(i64 noundef %x, i64 noundef %y, i8 noundef %count) {
entry:
 %n = and i8 %count, 3
 br label %head
head:
 %a = phi i64 [%x, %entry], [%b, %body]
 %b = phi i64 [%y, %entry], [%a, %body]
 %i = phi i8 [0, %entry], [%next, %body]
 %go = icmp ult i8 %i, %n
 br i1 %go, label %body, label %exit
body:
 %next = add nuw i8 %i, 1
 br label %head
exit:
 %r = sub i64 %a, %b
 ret i64 %r
})";
  constexpr char Parity[] = R"(
define i64 @f(i64 noundef %x, i64 noundef %y, i8 noundef %count) {
 %bit = and i8 %count, 1
 %odd = icmp ne i8 %bit, 0
 %a = sub i64 %x, %y
 %b = sub i64 %y, %x
 %r = select i1 %odd, i64 %b, i64 %a
 ret i64 %r
})";
  auto R = check(Loop, Parity);
  EXPECT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.CompletedPartitions, 4U);
}

TEST(LLVMScalarEquivalence, SwitchAndHighControlBitsUseCompleteDomain) {
  constexpr char Switch[] = R"(
define i64 @f(i64 noundef %x, i64 noundef %n) {
 %s = lshr i64 %n, 61
 %v = and i64 %s, 3
 switch i64 %v, label %d [ i64 0, label %a
                          i64 2, label %b ]
a: ret i64 %x
b:
 %r = xor i64 %x, 12
 ret i64 %r
d:
 %q = add i64 %x, 4
 ret i64 %q
})";
  constexpr char Select[] = R"(
define i64 @f(i64 noundef %x, i64 noundef %n) {
 %s = lshr i64 %n, 61
 %v = and i64 %s, 3
 %a = icmp eq i64 %v, 0
 %b = icmp eq i64 %v, 2
 %r = xor i64 %x, 12
 %q = add i64 %x, 4
 %u = select i1 %b, i64 %r, i64 %q
 %out = select i1 %a, i64 %x, i64 %u
 ret i64 %out
})";
  auto R = check(Switch, Select);
  EXPECT_EQ(R.Status, Status::Proved) << R.Diagnostic;
  EXPECT_EQ(R.ControlBits,
            (std::vector<LLVMScalarControlBit>{{1, 61}, {1, 62}}));
}

TEST(LLVMScalarEquivalence, ExtraPoisonProducingUpdateCannotBeIgnored) {
  constexpr char Good[] = R"(
define i8 @f(i8 noundef %x) {
 %bit = and i8 %x, 1
 %v = add nuw i8 %bit, 254
 ret i8 %v
})";
  std::string Bad = Good;
  Bad.insert(Bad.find("ret i8"), "%unused = add nuw i8 %v, 1\n");
  EXPECT_EQ(check(Good, Good).Status, Status::Proved);
  auto R = check(Good, Bad);
  EXPECT_EQ(R.Status, Status::Unproved);
  EXPECT_NE(R.Diagnostic.find("not defined"), std::string::npos);
  // The strict model also refuses poison hidden by an unused select.
  constexpr char Hidden[] = R"(
define i8 @f(i8 noundef %x) {
 %bad = add nuw i8 255, 1
 %unused = select i1 false, i8 %bad, i8 %x
 ret i8 %x
})";
  EXPECT_EQ(check(Hidden, Hidden).Status, Status::Unproved);
}

TEST(LLVMScalarEquivalence, WorkBudgetsIncludeFailedRefinementAttempts) {
  auto Baseline = check(Counted, Closed);
  ASSERT_EQ(Baseline.Status, Status::Proved) << Baseline.Diagnostic;
  LLVMScalarEquivalenceLimits L;
  L.MaxWork = Baseline.Work;
  EXPECT_EQ(check(Counted, Closed, L).Status, Status::Proved);
  --L.MaxWork;
  auto Short = check(Counted, Closed, L);
  EXPECT_EQ(Short.Status, Status::BudgetExceeded);
  EXPECT_EQ(Short.Work, L.MaxWork);
  L.MaxWork = 0;
  EXPECT_EQ(check(Counted, Closed, L).Status, Status::BudgetExceeded);
}

TEST(LLVMScalarEquivalence, AllResourceLimitsFailWithoutProof) {
  for (auto Field : {&LLVMScalarEquivalenceLimits::MaxPartitions,
                     &LLVMScalarEquivalenceLimits::MaxBlockVisits,
                     &LLVMScalarEquivalenceLimits::MaxSymbolicNodes}) {
    LLVMScalarEquivalenceLimits L;
    L.*Field = 0;
    EXPECT_EQ(check(Counted, Closed, L).Status, Status::BudgetExceeded);
  }
  LLVMScalarEquivalenceLimits L;
  L.MaxControlBits = 2;
  EXPECT_EQ(check(Counted, Closed, L).Status, Status::BudgetExceeded);
  L = {};
  L.MaxPartitions = 7;
  EXPECT_EQ(check(Counted, Closed, L).Status, Status::BudgetExceeded);
  for (auto Field : {&LLVMInterpreterModelLimits::MaxInputItems,
                     &LLVMInterpreterModelLimits::MaxBlocks,
                     &LLVMInterpreterModelLimits::MaxOperations,
                     &LLVMInterpreterModelLimits::MaxWork}) {
    L = {};
    L.Model.*Field = 0;
    EXPECT_EQ(check(Counted, Closed, L).Status, Status::BudgetExceeded);
  }
}

TEST(LLVMScalarEquivalence, NonterminationAndWideControlRemainUnproved) {
  constexpr char Forever[] = R"(
define i32 @f(i32 noundef %x) {
entry: br label %loop
loop: br label %loop
})";
  LLVMScalarEquivalenceLimits L;
  L.MaxBlockVisits = 8;
  EXPECT_EQ(check(Forever, Forever, L).Status, Status::BudgetExceeded);
  constexpr char Wide[] = R"(
define i64 @f(i64 noundef %x) {
 %c = icmp eq i64 %x, 0
 br i1 %c, label %a, label %b
a: ret i64 0
b: ret i64 %x
})";
  EXPECT_EQ(check(Wide, Wide).Status, Status::BudgetExceeded);
}

TEST(LLVMScalarEquivalence, SignaturesAndInputContractsMustAgree) {
  const char *Good = "define i32 @f(i32 noundef %x) { ret i32 %x }";
  for (const char *Bad :
       {"define i32 @f(i32 %x) { ret i32 %x }",
        "define i32 @f(i32 noundef range(i32 0, 8) %x) { ret i32 %x }",
        "define i64 @f(i32 noundef %x) { ret i64 0 }",
        "define i32 @f(i64 noundef %x) { ret i32 0 }",
        "define i32 @f(i32 noundef %x, i32 noundef %y) { ret i32 %x }"})
    EXPECT_EQ(check(Good, Bad).Status, Status::Unsupported) << Bad;
}
} // namespace neverd::analysis::scalar_test

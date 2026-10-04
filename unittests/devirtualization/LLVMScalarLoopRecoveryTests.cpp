//===- LLVMScalarLoopRecoveryTests.cpp - Loop recovery tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "neverd/analysis/LLVMScalarLoopRecovery.h"

#include "llvm/IR/Verifier.h"

#include <cstring>

namespace neverd::analysis::scalar_test {
namespace {
using RecoveryStatus = LLVMScalarLoopRecoveryStatus;
constexpr char Header[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %empty = icmp eq i8 %bound, 0
 br i1 %empty, label %exit, label %loop
loop:
 %i = phi i8 [0, %entry], [%next, %loop]
 %v = phi i32 [%x, %entry], [%sum, %loop]
 %sum = add i32 %v, 9
 %next = add nuw i8 %i, 1
 %done = icmp eq i8 %next, %bound
 br i1 %done, label %exit, label %loop
exit:
 %r = phi i32 [%x, %entry], [%sum, %loop]
 ret i32 %r
})";
constexpr char Prefix[] = R"(
define i16 @f(i16 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %empty = icmp eq i8 %bound, 0
 br i1 %empty, label %zero, label %peeled
zero: ret i16 %x
peeled:
 %first = xor i16 %x, 93
 %one = icmp eq i8 %bound, 1
 br i1 %one, label %exit, label %pre
pre: br label %loop
loop:
 %i = phi i8 [1, %pre], [%next, %loop]
 %v = phi i16 [%first, %pre], [%sum, %loop]
 %sum = xor i16 %v, 93
 %next = add nuw i8 %i, 1
 %done = icmp eq i8 %next, %bound
 br i1 %done, label %exit, label %loop
exit:
 %r = phi i16 [%first, %peeled], [%sum, %loop]
 ret i16 %r
})";
constexpr char Carriers[] = R"(
define i16 @f(i16 noundef %x, i8 noundef %n) {
entry:
 %bits = and i8 %n, 3
 %bound = add nuw i8 %bits, 1
 %first = add i16 %x, 6
 br label %outer
outer:
 %k = phi i8 [0, %entry], [%knext, %latch]
 %value = phi i16 [%first, %entry], [%first.next, %latch]
 %one = icmp eq i8 %bound, 1
 br i1 %one, label %after, label %inner
inner:
 %i = phi i8 [1, %outer], [%inext, %inner]
 %v = phi i16 [%value, %outer], [%sum, %inner]
 %sum = add i16 %v, 6
 %inext = add nuw i8 %i, 1
 %done = icmp eq i8 %inext, %bound
 br i1 %done, label %after, label %inner
after:
 %last = phi i16 [%value, %outer], [%sum, %inner]
 %knext = add nuw i8 %k, 1
 %stop = icmp eq i8 %knext, 2
 br i1 %stop, label %exit, label %latch
latch:
 %first.next = add i16 %last, 6
 br label %outer
exit: ret i16 %last
})";
constexpr char Parallel[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 br label %head
head:
 %i = phi i8 [0, %entry], [%next, %body]
 %scaled = phi i8 [4, %entry], [%snext, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %go = icmp ult i8 %i, %bound
 br i1 %go, label %body, label %exit
body:
 %wide = zext i8 %scaled to i32
 %sum = add i32 %v, %wide
 %next = add nuw i8 %i, 1
 %snext = add i8 %scaled, 11
 br label %head
exit: ret i32 %v
})";

constexpr char ZeroRegion[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry: br label %gate
gate:
 %bound = and i8 %n, 3
 %empty = icmp eq i8 %bound, 0
 br i1 %empty, label %zero, label %pre
zero:
 %zi = phi i8 [0, %gate], [%zn, %zero]
 %zv = phi i32 [%x, %gate], [%zx, %zero]
 %zw = zext i8 %zi to i32
 %zx = xor i32 %zv, %zw
 %zn = add nuw i8 %zi, 1
 %zd = icmp eq i8 %zn, 2
 br i1 %zd, label %exit, label %zero
pre: br label %outer
outer:
 %k = phi i8 [0, %pre], [%kn, %latch]
 %v = phi i32 [%x, %pre], [%adjusted, %latch]
 br label %inner
inner:
 %i = phi i8 [0, %outer], [%next, %inner]
 %iv = phi i32 [%v, %outer], [%sum, %inner]
 %sum = add i32 %iv, 23
 %next = add nuw i8 %i, 1
 %done = icmp eq i8 %next, %bound
 br i1 %done, label %after, label %inner
after:
 %kw = zext i8 %k to i32
 %adjusted = xor i32 %sum, %kw
 %stop = icmp eq i8 %k, 1
 br i1 %stop, label %exit, label %latch
latch:
 %kn = add nuw i8 %k, 1
 br label %outer
exit:
 %r = phi i32 [%zx, %zero], [%adjusted, %after]
 ret i32 %r
})";

// Independent scalar probes. The byte last index wraps on the skipped path;
// its zero extension plus one is not the original zero-trip boundary.
constexpr char WidenedLast[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 7
 %empty = icmp eq i8 0, %bound
 br i1 %empty, label %exit, label %pre
pre:
 %last.byte = add i8 %bound, -1
 %last = zext i8 %last.byte to i16
 br label %head
head:
 %i = phi i16 [0, %pre], [%next, %latch]
 %v = phi i32 [%x, %pre], [%sum, %latch]
 br label %body
body:
 %sum = add i32 %v, 37
 br label %latch
latch:
 %next = add nuw i16 %i, 1
 %done = icmp eq i16 %i, %last
 br i1 %done, label %exit, label %head
exit:
 %r = phi i32 [%x, %entry], [%sum, %latch]
 ret i32 %r
})";

constexpr char LateUnit[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bits = and i8 %n, 7
 %bound = zext i8 %bits to i32
 br label %head
head:
 %scaled = phi i32 [7, %entry], [%snext, %body]
 %i = phi i32 [0, %entry], [%next, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %go = icmp ult i32 %i, %bound
 br i1 %go, label %body, label %exit
body:
 %sum = add i32 %v, %scaled
 %snext = add i32 %scaled, 19
 %next = add nuw i32 %i, 1
 br label %head
exit: ret i32 %v
})";

constexpr char EntrySeed[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %seed = and i32 %x, 255
 br label %head
head:
 %carried = phi i32 [%seed, %entry], [%masked, %body]
 %i = phi i8 [0, %entry], [%next, %body]
 %v = phi i32 [%x, %entry], [%sum, %body]
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %masked = and i32 %carried, 255
 %sum = add i32 %v, %carried
 %next = add nuw i8 %i, 1
 br label %head
exit: ret i32 %v
})";

void replace(std::string &IR, llvm::StringRef From, llvm::StringRef To) {
  auto At = IR.find(From.str());
  ASSERT_NE(At, std::string::npos) << From.str();
  IR.replace(At, From.size(), To.str());
}

std::string narrowZeroRegion() {
  std::string IR = ZeroRegion;
  replace(IR, "%empty = icmp eq i8 %bound, 0",
          "%last.byte = add i8 %bound, -1\n"
          " %last = zext i8 %last.byte to i16\n"
          " %empty = icmp eq i8 %bound, 0");
  replace(IR, "%i = phi i8", "%i = phi i16");
  replace(IR, "%next = add nuw i8 %i, 1", "%next = add nuw i16 %i, 1");
  replace(IR, "%done = icmp eq i8 %next, %bound",
          "%done = icmp eq i16 %i, %last");
  return IR;
}

std::string text(const llvm::Module &M) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  M.print(OS, nullptr);
  return S;
}
LLVMScalarLoopRecoveryResult
recover(Source &Input, const LLVMScalarLoopRecoveryLimits &L = {}) {
  auto Before = Input.text();
  auto R = recoverLLVMScalarLoops(Input.function(), L);
  EXPECT_EQ(Input.text(), Before);
  EXPECT_LE(R.ConstructionWork, L.MaxConstructionWork);
  EXPECT_LE(R.ProofWork, L.MaxProofWork);
  EXPECT_LE(R.Candidates, L.MaxCandidates);
  if (R.Module) {
    EXPECT_EQ(R.Status, RecoveryStatus::Recovered);
    EXPECT_FALSE(llvm::verifyModule(*R.Module));
    auto Proof = checkLLVMScalarEquivalence(Input.function(),
                                            *R.Module->getFunction("f"));
    EXPECT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
  } else {
    EXPECT_NE(R.Status, RecoveryStatus::Recovered);
  }
  return R;
}
} // namespace

TEST(LLVMScalarLoopRecovery, SelfLatchBecomesHeaderTest) {
  Source Input(Header);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto IR = text(*R.Module);
  EXPECT_NE(IR.find("icmp ult i8"), std::string::npos);
  EXPECT_EQ(IR.find("%r = phi"), std::string::npos);
  EXPECT_GE(R.ProvedTransforms, 1U);
}

TEST(LLVMScalarLoopRecovery, RemovedPrefixRetainsAllDataAndZeroPath) {
  Source Input(Prefix);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%first ="), std::string::npos);
  EXPECT_EQ(text(*R.Module).find("peeled:"), std::string::npos);
  EXPECT_NE(text(*R.Module).find("zero:"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, RetimesBothPredecessorCarrierEdges) {
  Source Input(Carriers);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto IR = text(*R.Module);
  EXPECT_EQ(IR.find("%first ="), std::string::npos);
  EXPECT_EQ(IR.find("%first.next ="), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, WrongPeeledOperationCannotBeRemoved) {
  std::string IR = Carriers;
  IR.replace(IR.find("%first.next = add i16 %last, 6"),
             std::string("%first.next = add i16 %last, 6").size(),
             "%first.next = add i16 %last, 10");
  Source Input(IR);
  auto R = recover(Input);
  if (R.Module)
    EXPECT_NE(text(*R.Module).find("%first.next ="), std::string::npos);
  else
    EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
}

TEST(LLVMScalarLoopRecovery, ParallelInductionAllowsProvedFlags) {
  Source Input(Parallel);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto IR = text(*R.Module);
  EXPECT_EQ(IR.find("%scaled = phi"), std::string::npos);
  EXPECT_NE(IR.find("mul i8 %i, 11"), std::string::npos);
  EXPECT_NE(IR.find("add nuw i8"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, ExtraLastOverflowRejectsRotation) {
  Source Input(R"(
define i16 @f(i16 noundef %x) {
entry: br i1 false, label %exit, label %head
head:
 %i = phi i8 [254, %entry], [%next, %latch]
 %v = phi i16 [%x, %entry], [%sum, %latch]
 %sum = add i16 %v, 13
 %done = icmp eq i8 %i, 255
 br i1 %done, label %exit, label %latch
latch:
 %next = add nuw i8 %i, 1
 br label %head
exit:
 %r = phi i16 [%x, %entry], [%sum, %head]
 ret i16 %r
})");
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
  EXPECT_GT(R.Candidates, 0U);
}

TEST(LLVMScalarLoopRecovery, EqualityFallbackPreservesWraparound) {
  Source Input(R"(
define i16 @f(i16 noundef %x) {
entry: br i1 false, label %exit, label %head
head:
 %i = phi i8 [200, %entry], [%next, %head]
 %v = phi i16 [%x, %entry], [%sum, %head]
 %sum = add i16 %v, 13
 %next = add i8 %i, 1
 %done = icmp eq i8 %next, 150
 br i1 %done, label %exit, label %head
exit:
 %r = phi i16 [%x, %entry], [%sum, %head]
 ret i16 %r
})");
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_NE(text(*R.Module).find("icmp ne i8 %i, -106"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, ZeroTripRemovesAlternativeAndRelocatesBound) {
  Source Input(ZeroRegion);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto IR = text(*R.Module);
  EXPECT_EQ(IR.find("zero:"), std::string::npos);
  EXPECT_EQ(IR.find("gate:"), std::string::npos);
  EXPECT_NE(IR.find("%bound = and i8 %n, 3"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, DifferentZeroTripObservationIsRetained) {
  std::string IR = ZeroRegion;
  IR.replace(IR.find("%zw = zext i8 %zi to i32"),
             std::string("%zw = zext i8 %zi to i32").size(),
             "%shifted = add i8 %zi, 1\n%zw = zext i8 %shifted to i32");
  Source Input(IR);
  auto R = recover(Input);
  if (R.Module)
    EXPECT_NE(text(*R.Module).find("zero:"), std::string::npos);
  else
    EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
}

TEST(LLVMScalarLoopRecovery, WidenedLastIndexUsesOriginalGuardBoundary) {
  for (bool Inverted : {false, true}) {
    std::string IR = WidenedLast;
    if (Inverted) {
      replace(IR, "icmp eq i8 0, %bound", "icmp ne i8 %bound, 0");
      replace(IR, "br i1 %empty, label %exit, label %pre",
              "br i1 %empty, label %pre, label %exit");
    }
    Source Input(IR);
    auto R = recover(Input);
    ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
    // The three inferred last+1 candidates fail on the zero-trip partition.
    EXPECT_GT(R.Candidates, 3U);
    auto Result = text(*R.Module);
    EXPECT_NE(Result.find("zext i8 %bound to i16"), std::string::npos);
    EXPECT_NE(Result.find("icmp ult i16 %i"), std::string::npos);
    EXPECT_EQ(Result.find("%last.byte ="), std::string::npos);
  }
}

TEST(LLVMScalarLoopRecovery, DominatingGuardSurvivesNarrowLastIndex) {
  for (bool Inverted : {false, true}) {
    auto IR = narrowZeroRegion();
    if (Inverted) {
      replace(IR, "icmp eq i8 %bound, 0", "icmp ne i8 %bound, 0");
      replace(IR, "br i1 %empty, label %zero, label %pre",
              "br i1 %empty, label %pre, label %zero");
    }
    Source Input(IR);
    auto R = recover(Input);
    ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
    EXPECT_EQ(text(*R.Module).find("zero:"), std::string::npos);
    EXPECT_EQ(text(*R.Module).find("gate:"), std::string::npos);
  }
}

TEST(LLVMScalarLoopRecovery, GuardBoundaryIsAProposalNotAnAssumption) {
  std::string IR = WidenedLast;
  replace(IR, "%last.byte = add i8 %bound, -1",
          "%last.byte = add i8 %bound, 0");
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
  EXPECT_GE(R.Candidates, 6U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, SignedLastIndexRequiresProvedOrdering) {
  std::string IR = WidenedLast;
  replace(IR, "%bound = and i8 %n, 7",
          "%bits = and i8 %n, 3\n %bound = add i8 %bits, -128");
  replace(IR, "icmp eq i8 0, %bound", "icmp eq i8 -128, %bound");
  replace(IR, "zext i8 %last.byte", "sext i8 %last.byte");
  replace(IR, "phi i16 [0, %pre]", "phi i16 [-128, %pre]");
  replace(IR, "add nuw i16 %i, 1", "add i16 %i, 1");
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  // The unsigned order skips the wrapped boundary on the empty path and
  // still admits all negative nonempty limits. Equality would execute extra
  // iterations, and blindly zero-extending the original guard is different.
  EXPECT_NE(text(*R.Module).find("icmp ult i16 %i"), std::string::npos);
  EXPECT_NE(text(*R.Module).find("sext i8 %last.byte to i16"),
            std::string::npos);
}

TEST(LLVMScalarLoopRecovery, GuardRelocationCannotEraseHiddenDataOrPoison) {
  for (bool Poison : {false, true}) {
    auto IR = narrowZeroRegion();
    if (Poison) {
      // This update was only defined on the original nonempty path.
      replace(IR, "pre: br label %outer",
              "pre:\n %required = sub nuw i8 %bound, 1\n br label %outer");
      replace(IR, "%sum = add i32 %iv, 23",
              "%amount = zext i8 %required to i32\n"
              " %sum = add i32 %iv, %amount");
    } else {
      replace(IR, "%zx = xor i32 %zv, %zw",
              "%zx = xor i32 %zv, %zw\n"
              " %high = lshr i32 %x, 16\n"
              " %hidden = add i32 %zx, %high");
      replace(IR, "[%zx, %zero]", "[%hidden, %zero]");
    }
    Source Input(IR);
    auto R = recover(Input);
    if (R.Module)
      EXPECT_NE(text(*R.Module).find("zero:"), std::string::npos);
    else
      EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
  }
}

TEST(LLVMScalarLoopRecovery, UnitStepBaseDoesNotDependOnPhiOrder) {
  for (bool Reordered : {false, true}) {
    std::string IR = LateUnit;
    if (Reordered)
      replace(IR,
              " %scaled = phi i32 [7, %entry], [%snext, %body]\n"
              " %i = phi i32 [0, %entry], [%next, %body]",
              " %i = phi i32 [0, %entry], [%next, %body]\n"
              " %scaled = phi i32 [7, %entry], [%snext, %body]");
    Source Input(IR);
    auto R = recover(Input);
    ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
    auto Result = text(*R.Module);
    EXPECT_EQ(Result.find("%scaled = phi"), std::string::npos);
    EXPECT_NE(Result.find("%i = phi"), std::string::npos);
    EXPECT_NE(Result.find("mul i32 %i, 19"), std::string::npos);
  }
}

TEST(LLVMScalarLoopRecovery, DescendingUnitStepAlsoProvidesAnAffineBase) {
  std::string IR = LateUnit;
  replace(IR, "%bound = zext i8 %bits to i32",
          "%wide = zext i8 %bits to i32\n %bound = sub i32 6, %wide");
  replace(IR, "phi i32 [0, %entry]", "phi i32 [6, %entry]");
  replace(IR, "icmp ult i32 %i, %bound", "icmp ne i32 %i, %bound");
  replace(IR, "add nuw i32 %i, 1", "sub i32 %i, 1");
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto Result = text(*R.Module);
  EXPECT_EQ(Result.find("%scaled = phi"), std::string::npos);
  EXPECT_NE(Result.find("mul i32 %i, -19"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, LateUnitDoesNotAuthorizePoisonousSourceUpdate) {
  std::string IR = LateUnit;
  replace(IR, "add i32 %scaled, 19", "add nuw i32 %scaled, 2147483651");
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unsupported);
  EXPECT_EQ(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, GuardAndUnitSearchKeepCumulativeBudgets) {
  for (auto IR : {WidenedLast, LateUnit}) {
    Source Input(IR);
    auto Base = recover(Input);
    ASSERT_EQ(Base.Status, RecoveryStatus::Recovered);
    for (bool Proof : {false, true}) {
      LLVMScalarLoopRecoveryLimits L;
      auto &Budget = Proof ? L.MaxProofWork : L.MaxConstructionWork;
      Budget = Proof ? Base.ProofWork : Base.ConstructionWork;
      EXPECT_EQ(recover(Input, L).Status, RecoveryStatus::Recovered);
      --Budget;
      auto Short = recover(Input, L);
      EXPECT_NE(Short.Status, RecoveryStatus::Recovered);
      EXPECT_FALSE(Short.Module);
    }
  }
  Source Input(WidenedLast);
  LLVMScalarLoopRecoveryLimits L;
  L.MaxCandidates = 3;
  auto R = recover(Input, L);
  EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded);
  EXPECT_EQ(R.Candidates, 3U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, EntryValueCanReplaceAProvedRedundantCarrier) {
  Source Input(EntrySeed);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto IR = text(*R.Module);
  EXPECT_EQ(IR.find("%carried = phi"), std::string::npos);
  EXPECT_NE(IR.find("%seed = and i32 %x, 255"), std::string::npos);
  EXPECT_NE(IR.find("%i = phi"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, EntryAgreementCannotEraseLaterHighData) {
  std::string IR = EntrySeed;
  replace(IR, "%masked = and i32 %carried, 255",
          "%high = lshr i32 %x, 16\n %masked = or i32 %carried, %high");
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
  EXPECT_GT(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, SeedProposalsPreserveParallelSwaps) {
  Source Input(R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %other = xor i32 %x, 61
 br label %head
head:
 %a = phi i32 [%x, %entry], [%b, %body]
 %b = phi i32 [%other, %entry], [%a, %body]
 %i = phi i8 [0, %entry], [%next, %body]
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %next = add nuw i8 %i, 1
 br label %head
exit: ret i32 %a
})");
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
  EXPECT_GT(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, FailedSeedBatchSplitsBeforeAcceptingTwoCarriers) {
  std::string IR = EntrySeed;
  replace(IR, "%seed = and i32 %x, 255",
          "%seed = and i32 %x, 255\n %other.seed = and i32 %x, 65280");
  replace(IR,
          " %carried = phi i32 [%seed, %entry], [%masked, %body]\n"
          " %i = phi i8 [0, %entry], [%next, %body]\n"
          " %v = phi i32 [%x, %entry], [%sum, %body]",
          " %v = phi i32 [%x, %entry], [%sum, %body]\n"
          " %carried = phi i32 [%seed, %entry], [%masked, %body]\n"
          " %other = phi i32 [%other.seed, %entry], [%other.masked, %body]\n"
          " %i = phi i8 [0, %entry], [%next, %body]");
  replace(IR, "%sum = add i32 %v, %carried",
          "%other.masked = and i32 %other, 65280\n"
          " %both = add i32 %carried, %other\n %sum = add i32 %v, %both");
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  // Zero data cannot distinguish the live sum from either invariant. The
  // full batch must fail, then the two valid replacements can pass together.
  EXPECT_EQ(R.ProvedTransforms, 1U);
  auto Output = text(*R.Module);
  EXPECT_EQ(Output.find("%carried = phi"), std::string::npos);
  EXPECT_EQ(Output.find("%other = phi"), std::string::npos);
  EXPECT_NE(Output.find("%v = phi"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, SeedBatchChunksContinuePastTheFirstGroup) {
  std::string IR = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %seed = and i32 %x, 255
 br label %head
head:
)";
  for (unsigned I = 0; I < 35; ++I) {
    auto N = std::to_string(I);
    IR += "%field" + N + " = phi i32 [%seed, %entry], [%masked" + N +
          ", %body]\n";
  }
  IR += R"(
 %i = phi i8 [0, %entry], [%next, %body]
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
)";
  for (unsigned I = 0; I < 35; ++I) {
    auto N = std::to_string(I);
    IR += "%masked" + N + " = and i32 %field" + N + ", 255\n";
  }
  IR += "%next = add nuw i8 %i, 1\n br label %head\nexit:\n";
  std::string Sum = "%field0";
  for (unsigned I = 1; I < 35; ++I) {
    auto N = std::to_string(I);
    IR += "%sum" + N + " = add i32 " + Sum + ", %field" + N + "\n";
    Sum = "%sum" + N;
  }
  IR += "ret i32 " + Sum + "\n}";
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_GE(R.ProvedTransforms, 2U);
  EXPECT_EQ(text(*R.Module).find("%field"), std::string::npos);
}

TEST(LLVMScalarLoopRecovery, SeedReplacementMustPreserveEveryBackedge) {
  constexpr char IR[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %seed = and i32 %x, 255
 br label %head
head:
 %carried = phi i32 [%seed, %entry], [%left.value, %left], [%right.value, %right]
 %i = phi i8 [0, %entry], [%next, %left], [%next, %right]
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %next = add nuw i8 %i, 1
 %bit = and i8 %i, 1
 %odd = icmp ne i8 %bit, 0
 br i1 %odd, label %right, label %left
left:
 %left.value = and i32 %carried, 255
 br label %head
right:
 %right.value = and i32 %carried, 255
 br label %head
exit: ret i32 %carried
})";
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%carried = phi"), std::string::npos);
  for (bool Poison : {false, true}) {
    std::string Bad = IR;
    replace(Bad, "%right.value = and i32 %carried, 255",
            Poison ? "%right.value = add nuw i32 %carried, -1"
                   : "%right.value = xor i32 %carried, 43");
    Source Other(Bad);
    auto Kept = recover(Other);
    EXPECT_EQ(Kept.Status,
              Poison ? RecoveryStatus::Unsupported : RecoveryStatus::Unchanged);
    EXPECT_FALSE(Kept.Module);
  }
}

TEST(LLVMScalarLoopRecovery, EveryExternalEntryMustSupplyTheSameSeed) {
  constexpr char IR[] = R"(
define i32 @f(i32 noundef %x, i8 noundef %n) {
entry:
 %bound = and i8 %n, 3
 %seed = and i32 %x, 255
 %alternate = xor i32 %seed, 43
 %bit = and i8 %n, 1
 %choose = icmp eq i8 %bit, 0
 br i1 %choose, label %left, label %right
left: br label %head
right: br label %head
head:
 %carried = phi i32 [%seed, %left], [%seed, %right], [%masked, %body]
 %i = phi i8 [0, %left], [0, %right], [%next, %body]
 %more = icmp ult i8 %i, %bound
 br i1 %more, label %body, label %exit
body:
 %masked = and i32 %carried, 255
 %next = add nuw i8 %i, 1
 br label %head
exit: ret i32 %carried
})";
  Source Same(IR);
  auto R = recover(Same);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  EXPECT_EQ(text(*R.Module).find("%carried = phi"), std::string::npos);
  std::string Different = IR;
  replace(Different, "[%seed, %right]", "[%alternate, %right]");
  Source Other(Different);
  auto Kept = recover(Other);
  EXPECT_EQ(Kept.Status, RecoveryStatus::Unchanged);
  EXPECT_FALSE(Kept.Module);
}

TEST(LLVMScalarLoopRecovery, SeedSearchBudgetsRemainAtomic) {
  Source Input(EntrySeed);
  auto Base = recover(Input);
  ASSERT_EQ(Base.Status, RecoveryStatus::Recovered) << Base.Diagnostic;
  for (bool Proof : {false, true}) {
    LLVMScalarLoopRecoveryLimits L;
    auto &Budget = Proof ? L.MaxProofWork : L.MaxConstructionWork;
    Budget = Proof ? Base.ProofWork : Base.ConstructionWork;
    EXPECT_EQ(recover(Input, L).Status, RecoveryStatus::Recovered) << Proof;
    --Budget;
    auto Short = recover(Input, L);
    EXPECT_NE(Short.Status, RecoveryStatus::Recovered);
    EXPECT_FALSE(Short.Module);
  }
  LLVMScalarLoopRecoveryLimits L;
  L.MaxCandidates = 1;
  auto Short = recover(Input, L);
  EXPECT_EQ(Short.Status, RecoveryStatus::BudgetExceeded);
  EXPECT_FALSE(Short.Module);
}

TEST(LLVMScalarLoopRecovery, RefusesMemoryAndUndefinedInputContracts) {
  for (const char *IR :
       {"define i8 @f(i8 %x) { ret i8 %x }",
        "define i8 @f(ptr noundef %p) { %x = load i8, ptr %p\nret i8 %x }"}) {
    Source Input(IR);
    auto R = recover(Input);
    EXPECT_EQ(R.Status, RecoveryStatus::Unsupported);
    EXPECT_EQ(R.Candidates, 0U);
    EXPECT_FALSE(R.Diagnostic.empty());
  }
}

TEST(LLVMScalarLoopRecovery, ZeroBudgetsPublishNothing) {
  for (unsigned Which = 0; Which < 5; ++Which) {
    Source Input(Header);
    LLVMScalarLoopRecoveryLimits L;
    switch (Which) {
    case 0:
      L.MaxCandidates = 0;
      break;
    case 1:
      L.MaxTransforms = 0;
      break;
    case 2:
      L.MaxConstructionWork = 0;
      break;
    case 3:
      L.MaxProofWork = 0;
      break;
    case 4:
      L.Proof.MaxWork = 0;
      break;
    }
    auto R = recover(Input, L);
    EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded);
    EXPECT_FALSE(R.Module);
  }
}

TEST(LLVMScalarLoopRecovery, ExactAndShortCumulativeWork) {
  Source Input(Carriers);
  auto Base = recover(Input);
  ASSERT_EQ(Base.Status, RecoveryStatus::Recovered);
  for (bool Proof : {false, true}) {
    LLVMScalarLoopRecoveryLimits L;
    auto &Budget = Proof ? L.MaxProofWork : L.MaxConstructionWork;
    Budget = Proof ? Base.ProofWork : Base.ConstructionWork;
    auto Exact = recover(Input, L);
    EXPECT_EQ(Exact.Status, RecoveryStatus::Recovered) << Proof;
    --Budget;
    auto Short = recover(Input, L);
    EXPECT_NE(Short.Status, RecoveryStatus::Recovered) << Proof;
    EXPECT_FALSE(Short.Module);
  }
}

TEST(LLVMScalarLoopRecovery, CandidateAndTransformCapsAreCumulative) {
  Source Input(Carriers);
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

TEST(LLVMScalarLoopRecovery, IntrinsicsCloneWithoutUnrelatedDefinitions) {
  std::string IR = Header;
  IR.replace(IR.find("%sum = add i32 %v, 9"),
             std::string("%sum = add i32 %v, 9").size(),
             "%sum = call i32 @llvm.fshr.i32(i32 %v, i32 %v, i32 3)");
  IR += "\ndeclare i32 @llvm.fshr.i32(i32, i32, i32)\n"
        "define i32 @unrelated() { ret i32 123 }\n";
  Source Input(IR);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered);
  EXPECT_TRUE(R.Module->getFunction("llvm.fshr.i32"));
  EXPECT_FALSE(R.Module->getFunction("unrelated"));
}
TEST(LLVMScalarLoopRecovery, ConcreteScreenCannotEraseSymbolicHighData) {
  std::string IR = Carriers;
  IR.replace(IR.find("%first.next = add i16 %last, 6"),
             std::string("%first.next = add i16 %last, 6").size(),
             "%high = and i16 %x, -32768\n"
             "%changed = add i16 %last, %high\n"
             "%first.next = add i16 %changed, 6");
  Source Input(IR), WithoutHigh(Carriers);
  auto R = recover(Input);
  ASSERT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
  auto Unequal = checkLLVMScalarEquivalence(*R.Module->getFunction("f"),
                                            WithoutHigh.function());
  EXPECT_EQ(Unequal.Status, Status::Unproved);
}

TEST(LLVMScalarLoopRecovery, CounterAndDataWidthsAreDerivedFromIR) {
  for (const char *Width : {"i8", "i16", "i32", "i64"}) {
    SCOPED_TRACE(Width);
    for (auto Pair : {std::pair{Header, "i8"}, std::pair{Prefix, "i16"}}) {
      std::string IR = Pair.first;
      size_t At = 0;
      while ((At = IR.find(Pair.second, At)) != std::string::npos) {
        IR.replace(At, std::strlen(Pair.second), Width);
        At += std::strlen(Width);
      }
      Source Input(IR);
      auto R = recover(Input);
      EXPECT_EQ(R.Status, RecoveryStatus::Recovered) << R.Diagnostic;
    }
  }
}

TEST(LLVMScalarLoopRecovery, SourceControlBudgetCannotBeSampledAway) {
  Source Input(Header);
  LLVMScalarLoopRecoveryLimits L;
  L.Proof.MaxControlBits = 1;
  auto R = recover(Input, L);
  EXPECT_EQ(R.Status, RecoveryStatus::BudgetExceeded);
  EXPECT_EQ(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, SourcePoisonDoesNotAuthorizeReconstruction) {
  std::string IR = Parallel;
  IR.replace(IR.find("%snext = add i8 %scaled, 11"),
             std::string("%snext = add i8 %scaled, 11").size(),
             "%snext = add nuw i8 %scaled, 129");
  Source Input(IR);
  auto R = recover(Input);
  EXPECT_EQ(R.Status, RecoveryStatus::Unsupported);
  EXPECT_EQ(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

TEST(LLVMScalarLoopRecovery, MalformedPhiAndLoopFreeFunctionAreUnchanged) {
  Source Bad(R"(
define i8 @f(i8 noundef %x) {
entry: br label %exit
exit:
 %v = phi i8 [%x, %entry], [%x, %entry]
 ret i8 %v
})");
  EXPECT_EQ(recover(Bad).Status, RecoveryStatus::Unsupported);
  Source Plain("define i64 @f(i64 noundef %x) { ret i64 %x }");
  auto R = recover(Plain);
  EXPECT_EQ(R.Status, RecoveryStatus::Unchanged);
  EXPECT_EQ(R.Candidates, 0U);
  EXPECT_FALSE(R.Module);
}

class LLVMScalarLoopCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarLoopCompiled, OriginalAndRecoveredMatchIndependentOracles) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  struct Case {
    const char *IR;
    const char *Type;
    const char *Oracle;
  };
  auto NarrowZero = narrowZeroRegion();
  const Case Cases[] = {
      {Header, "uint32_t", "x + 9u * (n & 3u)"},
      {Carriers, "uint16_t", "x + 12u * ((n & 3u) + 1u)"},
      {ZeroRegion, "uint32_t", "(x + 46u * (n & 3u)) ^ 1u"},
      {Parallel, "uint32_t",
       "x + 4u * (n & 3u) + 11u * (n & 3u) * ((n & 3u) - 1u) / 2u"},
      {WidenedLast, "uint32_t", "x + 37u * (n & 7u)"},
      {NarrowZero.c_str(), "uint32_t", "(x + 46u * (n & 3u)) ^ 1u"},
      {LateUnit, "uint32_t",
       "x + 7u * (n & 7u) + 19u * (n & 7u) * ((n & 7u) - 1u) / 2u"},
      {EntrySeed, "uint32_t", "x + (x & 255u) * (n & 3u)"}};
  unsigned Number = 0;
  for (auto Case : Cases) {
    SCOPED_TRACE(Number++);
    Source Input(Case.IR);
    auto Recovered = recover(Input);
    ASSERT_EQ(Recovered.Status, RecoveryStatus::Recovered)
        << Recovered.Diagnostic;
    auto Harness = tmpFile("oracle.c");
    std::ofstream(Harness)
        << "#include <stdint.h>\nextern " << Case.Type << " f(" << Case.Type
        << ", uint8_t);\nint main(void) {\n"
           "uint32_t random = 0xffffffffu;\n"
           "for (unsigned n = 0; n < 256; ++n) {\n"
           "for (unsigned sample = 0; sample < 96; ++sample) {\n"
           "random = random * 1664525u + 1013904223u;\n"
           "uint32_t x = sample == 0 ? 0 : sample == 1 ? 0xffffffffu : "
           "random;\n"
        << Case.Type << " expected = (" << Case.Type << ")(" << Case.Oracle
        << ");\n"
        << "if (f((" << Case.Type
        << ")x, (uint8_t)n) != expected) return 1;\n"
           "}} return 0; }\n";
    for (auto *M : {Input.Module.get(), Recovered.Module.get()}) {
      auto IR = tmpFile("value.ll");
      std::ofstream(IR) << text(*M);
      for (const char *Level : {"-O0", "-O2"}) {
        SCOPED_TRACE(Level);
        auto Binary =
            tmpFile(std::string("oracle") + neverd::test::executableSuffix());
        auto Built =
            exec(NEVERD_TEST_CLANG,
                 {Level, IR.string(), Harness.string(), "-o", Binary.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        auto Ran = exec(Binary.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err;
      }
    }
  }
}
} // namespace neverd::analysis::scalar_test

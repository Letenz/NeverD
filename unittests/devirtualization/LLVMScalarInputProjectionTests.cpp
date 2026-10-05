//===- LLVMScalarInputProjectionTests.cpp - Explicit input demand ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../lift/NeverDLiftFixture.h"
#include "LLVMScalarEquivalenceTest.h"

#include "neverd/analysis/LLVMScalarInputProjection.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::analysis::scalar_test {
using InputProjection = LLVMScalarInputProjectionResult;

static llvm::Function &definition(llvm::Module &M) {
  for (auto &F : M)
    if (!F.isDeclaration())
      return F;
  llvm_unreachable("test requires a definition");
}

static InputProjection
inputs(Source &S, const LLVMScalarInputProjectionLimits &Limits = {}) {
  auto Before = S.text();
  auto R = projectLLVMScalarInputs(definition(*S.Module), Limits);
  EXPECT_EQ(S.text(), Before);
  if (R.Module)
    EXPECT_FALSE(llvm::verifyModule(*R.Module));
  else {
    EXPECT_TRUE(R.Arguments.empty());
    EXPECT_EQ(R.RemovedArguments, 0U);
  }
  return R;
}

// Re-embed every candidate argument into the complete original signature.
// Unlike argument deletion, this clone maps every source parameter.
static std::unique_ptr<llvm::Module> embed(const llvm::Function &Original,
                                           const InputProjection &R) {
  auto M = llvm::CloneModule(*Original.getParent());
  auto &Full = definition(*M);
  auto &Candidate = definition(*R.Module);
  Full.deleteBody();
  llvm::ValueToValueMapTy Map;
  Map[&Candidate] = &Full;
  for (unsigned N = 0; N != R.Arguments.size(); ++N)
    Map[Candidate.getArg(N)] = Full.getArg(R.Arguments[N]);
  for (const auto &F : *R.Module)
    if (&F != &Candidate)
      Map[&F] = M->getFunction(F.getName());
  llvm::SmallVector<llvm::ReturnInst *, 8> Returns;
  llvm::CloneFunctionInto(&Full, &Candidate, Map,
                          llvm::CloneFunctionChangeType::DifferentModule,
                          Returns);
  Full.setAttributes(Original.getAttributes());
  EXPECT_FALSE(llvm::verifyModule(*M));
  return M;
}

TEST(LLVMScalarInputs, KeepsOriginalOrderTypesAndCompleteSignatureProof) {
  for (const char *Triple :
       {"x86_64-linux-gnu", "aarch64-linux-gnu", "aarch64_be-linux-gnu"}) {
    Source S(std::string("target triple = \"") + Triple + R"("
define i32 @f(i8 noundef %pad, i32 noundef %x, i64 noundef %other,
              i1 noundef %choose, i32 noundef %unused) {
  %a = add i32 %x, 6
  %b = sub i32 %x, 5
  %r = select i1 %choose, i32 %a, i32 %b
  ret i32 %r
})");
    auto R = inputs(S);
    ASSERT_EQ(R.Status, InputProjection::Projected) << R.Diagnostic;
    EXPECT_EQ(R.Arguments, (std::vector<unsigned>{1, 3}));
    EXPECT_EQ(R.RemovedArguments, 3U);
    auto &F = definition(*R.Module);
    EXPECT_EQ(F.arg_size(), 2U);
    EXPECT_TRUE(F.getArg(0)->getType()->isIntegerTy(32));
    EXPECT_TRUE(F.getArg(1)->getType()->isIntegerTy(1));
    EXPECT_EQ(F.getArg(0)->getName(), "x");
    EXPECT_EQ(F.getArg(1)->getName(), "choose");
    for (const auto &A : F.args())
      EXPECT_TRUE(A.hasAttribute(llvm::Attribute::NoUndef));
    EXPECT_EQ(R.Module->getTargetTriple(), S.Module->getTargetTriple());
    auto Full = embed(S.function(), R);
    auto Proof = checkLLVMScalarEquivalence(S.function(), definition(*Full));
    EXPECT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
    auto Again = projectLLVMScalarInputs(F);
    ASSERT_EQ(Again.Status, InputProjection::Projected);
    EXPECT_EQ(Again.Arguments, (std::vector<unsigned>{0, 1}));
    EXPECT_EQ(Again.RemovedArguments, 0U);
  }
}

TEST(LLVMScalarInputs, ZeroAndUnnamedInterfacesRemainValid) {
  Source S(R"(define i64 @f(i32 noundef %unused) { ret i64 19 })");
  S.function().setName("");
  auto R = inputs(S);
  ASSERT_EQ(R.Status, InputProjection::Projected) << R.Diagnostic;
  EXPECT_TRUE(R.Arguments.empty());
  EXPECT_EQ(R.RemovedArguments, 1U);
  auto &F = definition(*R.Module);
  EXPECT_TRUE(F.arg_empty());
  auto Full = embed(definition(*S.Module), R);
  EXPECT_EQ(checkLLVMScalarEquivalence(definition(*S.Module), definition(*Full))
                .Status,
            Status::Proved);
}

TEST(LLVMScalarInputs, DeadArithmeticAndAssumptionsStillDemandInputs) {
  const char *Bodies[] = {
      R"(define i8 @f(i8 noundef %x, i8 noundef %y, i8 noundef %unused) {
        %dead = add nuw i8 %y, 1
        ret i8 %x
      })",
      R"(declare void @llvm.assume(i1)
      define i8 @f(i8 noundef %x, i8 noundef %y, i8 noundef %unused) {
        %ok = icmp ne i8 %y, 255
        call void @llvm.assume(i1 %ok)
        ret i8 %x
      })"};
  for (const auto *Body : Bodies) {
    Source S(Body);
    auto R = inputs(S);
    ASSERT_EQ(R.Status, InputProjection::Projected) << R.Diagnostic;
    EXPECT_EQ(R.Arguments, (std::vector<unsigned>{0, 1}));
    EXPECT_EQ(R.RemovedArguments, 1U);
    auto &F = definition(*R.Module);
    EXPECT_FALSE(F.getArg(1)->use_empty());
    EXPECT_EQ(checkLLVMScalarEquivalence(F, F).Status, Status::Unproved);
  }
}

TEST(LLVMScalarInputs, MappingCannotAuthorizeAnAlteredResult) {
  Source S(R"(define i32 @f(i32 noundef %unused, i32 noundef %x) {
    %r = add i32 %x, 9
    ret i32 %r
  })");
  auto R = inputs(S);
  ASSERT_EQ(R.Status, InputProjection::Projected);
  auto &F = definition(*R.Module);
  F.front().front().setOperand(
      1, llvm::ConstantInt::get(F.getArg(0)->getType(), 10));
  auto Full = embed(S.function(), R);
  EXPECT_EQ(checkLLVMScalarEquivalence(S.function(), definition(*Full)).Status,
            Status::Unproved);
}

TEST(LLVMScalarInputs, RefusesUnmodeledContractsAndPackagingChanges) {
  const char *Bodies[] = {
      "define i32 @f(i32 %unused) { ret i32 0 }",
      "define i32 @f(ptr noundef %p) { ret i32 0 }",
      "define i32 @f(i32 noundef %x, ...) { ret i32 %x }",
      "define i128 @f(i32 noundef %x) { ret i128 0 }",
      "declare i32 @other(i32)\n"
      "define i32 @f(i32 noundef %x) { %r = call i32 @other(i32 %x) ret i32 %r "
      "}",
      "define range(i32 0, 32) i32 @f(i32 noundef %x) { ret i32 0 }",
      "define i32 @f(i32 noundef %x) {"
      " %dead = insertvalue {i32, i32} zeroinitializer, i32 %x, 0 ret i32 0 }"};
  for (const auto *Body : Bodies) {
    SCOPED_TRACE(Body);
    Source S(Body);
    auto R = inputs(S);
    EXPECT_EQ(R.Status, InputProjection::Unsupported) << R.Diagnostic;
    EXPECT_FALSE(R.Module);
  }
  Source Metadata("define i32 @f(i32 noundef %x) { ret i32 %x }");
  Metadata.function().setMetadata("unmodeled",
                                  llvm::MDNode::get(Metadata.Context, {}));
  EXPECT_EQ(inputs(Metadata).Status, InputProjection::Unsupported);
}

TEST(LLVMScalarInputs, ExactAndShortBudgetsDoNotPublishPartialMappings) {
  Source S(R"(define i32 @f(i32 noundef %unused, i32 noundef %x) {
    %r = xor i32 %x, 13
    ret i32 %r
  })");
  auto R = inputs(S);
  ASSERT_EQ(R.Status, InputProjection::Projected);
  ASSERT_GT(R.ConstructionWork, 1U);
  LLVMScalarInputProjectionLimits Limits;
  Limits.MaxConstructionWork = R.ConstructionWork;
  EXPECT_EQ(inputs(S, Limits).Status, InputProjection::Projected);
  --Limits.MaxConstructionWork;
  auto Short = inputs(S, Limits);
  EXPECT_EQ(Short.Status, InputProjection::BudgetExceeded);
  EXPECT_FALSE(Short.Module);
  EXPECT_LE(Short.ConstructionWork, Limits.MaxConstructionWork);
  Limits.MaxConstructionWork = 0;
  EXPECT_EQ(inputs(S, Limits).Status, InputProjection::BudgetExceeded);
}

class LLVMScalarInputsCompiled : public NeverDLiftTest {};

TEST_F(LLVMScalarInputsCompiled, LoopInputsMatchIndependentO0O2Oracle) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "Configured Clang is unavailable";
  Source S(R"(
define i32 @f(i32 noundef %a, i32 noundef %x, i32 noundef %b,
              i32 noundef %n, i32 noundef %c) {
entry:
  %limit = and i32 %n, 7
  br label %head
head:
  %i = phi i32 [0, %entry], [%next, %body]
  %value = phi i32 [%x, %entry], [%sum, %body]
  %more = icmp ult i32 %i, %limit
  br i1 %more, label %body, label %exit
body:
  %sum = add i32 %value, 11
  %next = add nuw nsw i32 %i, 1
  br label %head
exit:
  ret i32 %value
})");
  auto R = inputs(S);
  ASSERT_EQ(R.Status, InputProjection::Projected) << R.Diagnostic;
  EXPECT_EQ(R.Arguments, (std::vector<unsigned>{1, 3}));
  auto Full = embed(S.function(), R);
  auto Proof = checkLLVMScalarEquivalence(S.function(), definition(*Full));
  ASSERT_EQ(Proof.Status, Status::Proved) << Proof.Diagnostic;
  auto Original = tmpFile("original.ll"), Reduced = tmpFile("reduced.ll");
  S.function().setName("original");
  std::ofstream(Original) << S.text();
  definition(*R.Module).setName("reduced");
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  R.Module->print(OS, nullptr);
  std::ofstream(Reduced) << Text;
  auto Harness = tmpFile("oracle.c");
  std::ofstream(Harness) << R"(
#include <stdint.h>
uint32_t original(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t reduced(uint32_t, uint32_t);
int main(void) {
  uint32_t random = 0x73c1839u;
  const uint32_t edge[] = {0, 1, UINT32_MAX, UINT32_MAX-32, 0x80000000, 0x7fffffff};
  for (unsigned k=0; k<8192; ++k) {
    random = random*1664525u+1013904223u;
    uint32_t x = k<1536 ? edge[k/256] : random;
    uint32_t n = k<1536 ? k : random>>8;
    uint32_t expected = x+11u*(n&7);
    for (unsigned variant=0; variant<3; ++variant) {
      random = random*1664525u+1013904223u;
      if (original(random,x,~random,n,random^0x514183u)!=expected ||
          reduced(x,n)!=expected) return 1;
    }
  }
  return 0;
})";
  for (const char *Level : {"-O0", "-O2"}) {
    auto Program = tmpFile("input-oracle");
    auto Built =
        exec(NEVERD_TEST_CLANG,
             {Level, "-fsanitize=undefined", "-fsanitize-trap=undefined",
              Original.string(), Reduced.string(), Harness.string(), "-o",
              Program.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    auto Ran = exec(Program.string(), {});
    EXPECT_TRUE(Ran.ok()) << Ran.err;
  }
}
} // namespace neverd::analysis::scalar_test

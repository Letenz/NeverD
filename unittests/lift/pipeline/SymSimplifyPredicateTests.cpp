//===- SymSimplifyPredicateTests.cpp - Exact modular truth sets
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/pass/ir/simplify/SymSimplifyPass.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <limits>

using namespace neverd;
namespace {

SymSimplifyOptions predicatesOnly() {
  SymSimplifyOptions Opts;
  Opts.MinMeasuredNodes = std::numeric_limits<size_t>::max();
  Opts.MaxFiniteValueWork = 0;
  return Opts;
}

std::string print(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  return Text;
}

unsigned count(const llvm::Function &F) {
  unsigned N = 0;
  for (const auto &BB : F)
    N += BB.size();
  return N;
}

std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &C, llvm::StringRef IR) {
  llvm::SMDiagnostic Error;
  auto M = llvm::parseAssemblyString(IR, Error, C);
  if (!M) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    Error.print("predicate-test", OS);
    ADD_FAILURE() << Text;
  }
  return M;
}

llvm::Function *signPair(llvm::Module &M, unsigned Width, llvm::StringRef Name,
                         llvm::APInt Left, llvm::APInt Right, unsigned Negated,
                         bool Union, bool Invert = false) {
  auto &C = M.getContext();
  auto *T = llvm::IntegerType::get(C, Width);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(C), {T}, false),
      llvm::Function::ExternalLinkage, Name, M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  auto *X = F->getArg(0);
  auto *A = B.CreateAdd(Negated & 1 ? B.CreateNeg(X) : X,
                        llvm::ConstantInt::get(T, Left));
  auto *D = B.CreateSub(llvm::ConstantInt::get(T, Right),
                        Negated & 2 ? X : B.CreateNeg(X));
  auto *Bits = Union ? B.CreateOr(A, D) : B.CreateAnd(A, D);
  auto *Cmp = B.CreateICmpSLT(Bits, llvm::ConstantInt::get(T, 0));
  llvm::Value *Result = Invert ? B.CreateNot(Cmp) : Cmp;
  B.CreateRet(B.CreateZExt(Result, B.getInt32Ty()));
  return F;
}

llvm::Function *encoded(llvm::Module &M, unsigned Width, llvm::StringRef Name,
                        unsigned Kind) {
  const llvm::APInt Offset(Width, 9);
  const auto Limit = Kind & 1 ? llvm::APInt::getSignedMinValue(Width) + 1
                              : llvm::APInt::getSignedMaxValue(Width);
  return signPair(M, Width, Name, Offset, Offset + Limit, Kind & 2 ? 3 : 0,
                  !(Kind & 1), Kind & 4);
}

// An independent small-integer evaluator: no ConstantRange, LLVM constant
// folding or symbolic engine calls. It also evaluates the original graph.
uint64_t evaluate(llvm::Value *V, uint64_t X) {
  if (llvm::isa<llvm::Argument>(V))
    return X;
  if (const auto *C = llvm::dyn_cast<llvm::ConstantInt>(V))
    return C->getZExtValue();
  const auto *I = llvm::cast<llvm::Instruction>(V);
  const unsigned Width = V->getType()->getIntegerBitWidth();
  const uint64_t Mask = (uint64_t(1) << Width) - 1;
  const uint64_t A = evaluate(I->getOperand(0), X);
  if (I->getOpcode() == llvm::Instruction::ZExt)
    return A;
  const uint64_t B = evaluate(I->getOperand(1), X);
  switch (I->getOpcode()) {
  case llvm::Instruction::Add:
    return (A + B) & Mask;
  case llvm::Instruction::Sub:
    return (A - B) & Mask;
  case llvm::Instruction::And:
    return A & B;
  case llvm::Instruction::Or:
    return A | B;
  case llvm::Instruction::Xor:
    return A ^ B;
  case llvm::Instruction::ICmp: {
    const auto *Cmp = llvm::cast<llvm::ICmpInst>(I);
    const unsigned InputWidth =
        I->getOperand(0)->getType()->getIntegerBitWidth();
    const uint64_t Sign = uint64_t(1) << (InputWidth - 1);
    switch (Cmp->getPredicate()) {
    case llvm::CmpInst::ICMP_EQ:
      return A == B;
    case llvm::CmpInst::ICMP_NE:
      return A != B;
    case llvm::CmpInst::ICMP_ULT:
      return A < B;
    case llvm::CmpInst::ICMP_ULE:
      return A <= B;
    case llvm::CmpInst::ICMP_UGT:
      return A > B;
    case llvm::CmpInst::ICMP_UGE:
      return A >= B;
    case llvm::CmpInst::ICMP_SLT:
      return (A ^ Sign) < (B ^ Sign);
    case llvm::CmpInst::ICMP_SLE:
      return (A ^ Sign) <= (B ^ Sign);
    case llvm::CmpInst::ICMP_SGT:
      return (A ^ Sign) > (B ^ Sign);
    case llvm::CmpInst::ICMP_SGE:
      return (A ^ Sign) >= (B ^ Sign);
    default:
      break;
    }
    break;
  }
  default:
    break;
  }
  ADD_FAILURE() << "Unexpected evaluator opcode " << I->getOpcodeName();
  return 0;
}

llvm::Value *returned(llvm::Function &F) {
  return llvm::cast<llvm::ReturnInst>(F.getEntryBlock().getTerminator())
      ->getReturnValue();
}

} // namespace

TEST(SymSimplifyPredicates, RecoversShiftedAndNegatedConditionsAtWideWidths) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 512u})
    for (unsigned Kind = 0; Kind != 8; ++Kind) {
      SCOPED_TRACE(::testing::Message() << Width << ":" << Kind);
      llvm::LLVMContext C;
      llvm::Module M("domains", C);
      auto *F = encoded(M, Width, "f", Kind);
      const unsigned Before = count(*F);
      EXPECT_GT(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u)
          << print(*F);
      EXPECT_LT(count(*F), Before);
      EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
      EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
    }
}

TEST(SymSimplifyPredicates, ExhaustsAllFourBitSignsOffsetsAndInputs) {
  unsigned Rewritten = 0;
  for (unsigned A = 0; A != 16; ++A)
    for (unsigned B = 0; B != 16; ++B)
      for (unsigned Negated = 0; Negated != 4; ++Negated)
        for (bool Union : {false, true}) {
          llvm::LLVMContext C;
          llvm::Module M("exhaustive", C);
          auto *F = signPair(M, 4, "f", llvm::APInt(4, A), llvm::APInt(4, B),
                             Negated, Union);
          uint64_t Expected[16];
          for (unsigned X = 0; X != 16; ++X) {
            const unsigned L = ((Negated & 1 ? -X : X) + A) & 15;
            const unsigned R = ((Negated & 2 ? -X : X) + B) & 15;
            Expected[X] = ((Union ? L | R : L & R) >> 3) & 1;
            ASSERT_EQ(evaluate(returned(*F), X), Expected[X]);
          }
          const unsigned Before = count(*F);
          Rewritten += SymSimplifyPass::simplify(*F, predicatesOnly()) != 0;
          ASSERT_LE(count(*F), Before);
          for (unsigned X = 0; X != 16; ++X)
            ASSERT_EQ(evaluate(returned(*F), X), Expected[X])
                << A << ":" << B << ":" << Negated << ":" << Union << ":" << X
                << "\n"
                << print(*F);
        }
  EXPECT_GT(Rewritten, 512u);
}

TEST(SymSimplifyPredicates, CombinesBooleanIntervalsAndCommutedConstants) {
  const char *Bodies[] = {
      "%a = add i8 37, %x\n%b = icmp ult i8 %a, 20\n"
      "%c = icmp ugt i8 %a, 4\n%d = and i1 %b, %c\n%r = xor i1 true, %d",
      "%a = sub i8 17, %x\n%b = icmp slt i8 0, %a\n"
      "%c = icmp sge i8 12, %a\n%r = and i1 %b, %c",
      "%a = xor i8 -1, %x\n%b = add i8 %a, 7\n%c = add i8 %b, 127\n"
      "%d = or i8 %b, %c\n%e = xor i8 -128, %d\n%r = icmp sge i8 %e, 0",
  };
  for (const char *Body : Bodies) {
    llvm::LLVMContext C;
    auto M = parse(C, "define i1 @f(i8 %x) {\n" + std::string(Body) +
                          "\nret i1 %r\n}");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    uint64_t Expected[256];
    for (unsigned X = 0; X != 256; ++X)
      Expected[X] = evaluate(returned(*F), X);
    ASSERT_GT(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u) << print(*F);
    for (unsigned X = 0; X != 256; ++X)
      ASSERT_EQ(evaluate(returned(*F), X), Expected[X]) << Body << ":" << X;
  }
}

TEST(SymSimplifyPredicates, RefusesDisconnectedOrConstantTruthSets) {
  for (llvm::StringRef Op : {"or", "and"}) {
    llvm::LLVMContext C;
    auto M = parse(C, "define i1 @f(i8 %x) {\n%a = icmp ult i8 %x, 5\n"
                      "%b = icmp eq i8 %x, 32\n%r = " +
                          Op.str() + " i1 %a, %b\nret i1 %r\n}");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i1 @f(i8 %x) {
  %a = add i8 %x, -128
  %b = or i8 %x, %a
  %r = icmp slt i8 %b, 0
  ret i1 %r
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  const std::string Before = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyPredicates, AnnotationsRemainOpaqueAndRetainPoisonDependence) {
  const char *Bodies[] = {
      "%a = add nsw i8 %x, 127\n%b = or i8 %x, %a\n%r = icmp slt i8 %b, 0",
      "%a = add nuw i8 %x, 127\n%b = or i8 %x, %a\n%r = icmp slt i8 %b, 0",
      "%a = add i8 %x, 127\n%b = or disjoint i8 %x, %a\n%r = icmp slt i8 %b, 0",
      "%a = add i8 %x, 127\n%b = or i8 %x, %a\n%r = icmp samesign slt i8 %b, 0",
  };
  for (const char *Body : Bodies) {
    llvm::LLVMContext C;
    auto M = parse(C, "define i1 @f(i8 %x) {\n" + std::string(Body) +
                          "\nret i1 %r\n}");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i1 @f(i8 %x) {
  %anchor = add nsw i8 %x, 7
  %a = add i8 %anchor, 127
  %b = or i8 %anchor, %a
  %r = icmp slt i8 %b, 0
  ret i1 %r
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  EXPECT_GT(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
  EXPECT_NE(print(*F).find("add nsw"), std::string::npos);
  EXPECT_NE(print(*F).find("icmp ne i8 %anchor, 0"), std::string::npos);
}

TEST(SymSimplifyPredicates, IndependentInputsReadsAndFreezesDoNotCorrelate) {
  const char *Inputs[] = {
      "%x = load volatile i8, ptr %p\n%y = load volatile i8, ptr %p",
      "%x = freeze i8 undef\n%y = freeze i8 undef",
      "%x = trunc i16 %first to i8\n%y = trunc i16 %second to i8",
  };
  for (const char *Input : Inputs) {
    llvm::LLVMContext C;
    auto M = parse(C, "define i1 @f(ptr %p, i16 %first, i16 %second) {\n" +
                          std::string(Input) +
                          "\n%a = add i8 %y, 127\n%b = or i8 %x, %a\n"
                          "%r = icmp slt i8 %b, 0\nret i1 %r\n}");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
}

TEST(SymSimplifyPredicates, RejectsHiddenUndefinedEdgesButAcceptsOneFreeze) {
  for (llvm::StringRef Input : {"undef", "poison"}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
define i1 @f(i1 %c) {
entry:
  br i1 %c, label %left, label %join
left:
  br label %join
join:
  %x = phi i8 [ 9, %entry ], [ )" +
                          Input.str() + R"(, %left ]
  %a = add i8 %x, 127
  %b = or i8 %x, %a
  %r = icmp slt i8 %b, 0
  ret i1 %r
})");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i1 @f() {
  %x = freeze i8 undef
  %a = add i8 %x, 127
  %b = or i8 %x, %a
  %r = icmp slt i8 %b, 0
  ret i1 %r
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  EXPECT_GT(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
  EXPECT_NE(print(*F).find("freeze i8 undef"), std::string::npos);
}

TEST(SymSimplifyPredicates, SharedUsesCannotPayForANonShrinkingRewrite) {
  llvm::LLVMContext C;
  llvm::Module M("shared", C);
  auto *F = encoded(M, 64, "f", 0);
  auto *Sink = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(C),
                              {llvm::Type::getInt64Ty(C)}, false),
      llvm::Function::ExternalLinkage, "observe", M);
  llvm::SmallVector<llvm::Instruction *, 8> Observed;
  for (auto &I : F->getEntryBlock())
    if (I.getType()->isIntegerTy(64))
      Observed.push_back(&I);
  llvm::IRBuilder<> B(F->getEntryBlock().getTerminator());
  for (auto *I : Observed)
    B.CreateCall(Sink, {I});
  const std::string Before = print(*F);
  auto Opts = predicatesOnly();
  Opts.MinInstructionsSaved = 0;
  EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyPredicates, BudgetSavingsAndStampFailuresLeaveIRIntact) {
  for (size_t Limit : {size_t(0), size_t(1), size_t(16)}) {
    llvm::LLVMContext C;
    llvm::Module M("budget", C);
    auto *F = encoded(M, 64, "f", 0);
    const std::string Before = print(*F);
    auto Opts = predicatesOnly();
    Opts.MaxPredicateWork = Limit;
    EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
    EXPECT_EQ(print(*F), Before);
  }
  llvm::LLVMContext C;
  llvm::Module M("policy", C);
  auto *F = encoded(M, 64, "f", 0);
  const std::string Before = print(*F);
  auto Opts = predicatesOnly();
  Opts.MinInstructionsSaved = std::numeric_limits<size_t>::max();
  EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
  EXPECT_EQ(print(*F), Before);
  F->addFnAttr(kObfuscatedFnAttr);
  const std::string Stamped = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
  EXPECT_EQ(print(*F), Stamped);
}

TEST(SymSimplifyPredicates, StandalonePhaseReportsExactAndShortDerivationWork) {
  llvm::LLVMContext C;
  llvm::Module M("standalone", C);
  auto *F = encoded(M, 64, "f", 0);
  const auto Base = SymSimplifyPass::simplifyPredicates(*F);
  ASSERT_EQ(Base.Rewrites, 1U);
  ASSERT_GT(Base.Work, 0U);
  EXPECT_FALSE(Base.WorkLimitExceeded);
  for (size_t Limit : {size_t(0), Base.Work - 1, Base.Work}) {
    SCOPED_TRACE(Limit);
    llvm::Module Copy("bounded", C);
    auto *Original = encoded(Copy, 64, "f", 0);
    const auto Before = print(*Original);
    SymSimplifyOptions Options;
    Options.MaxPredicateWork = Limit;
    const auto R = SymSimplifyPass::simplifyPredicates(*Original, Options);
    EXPECT_EQ(R.Work, Limit);
    EXPECT_EQ(R.WorkLimitExceeded, Limit != 0 && Limit < Base.Work);
    if (Limit == Base.Work) {
      EXPECT_EQ(R.Rewrites, 1U);
      EXPECT_EQ(print(*Original), print(*F));
    } else {
      EXPECT_EQ(R.Rewrites, 0U);
      EXPECT_EQ(print(*Original), Before);
    }
  }
}

TEST(SymSimplifyPredicates, StandalonePhaseRetainsFullPassPolicy) {
  llvm::LLVMContext C;
  llvm::Module Left("left", C), Right("right", C);
  auto *F = encoded(Left, 32, "f", 4);
  auto *G = encoded(Right, 32, "f", 4);
  const auto R = SymSimplifyPass::simplifyPredicates(*F);
  EXPECT_EQ(R.Rewrites, SymSimplifyPass::simplify(*G, predicatesOnly()));
  EXPECT_EQ(print(*F), print(*G));

  llvm::Module Policy("policy", C);
  auto *Stamped = encoded(Policy, 64, "f", 0);
  Stamped->addFnAttr(kObfuscatedFnAttr);
  const auto Before = print(*Stamped);
  const auto Refused = SymSimplifyPass::simplifyPredicates(*Stamped);
  EXPECT_EQ(Refused.Rewrites, 0U);
  EXPECT_EQ(Refused.Work, 0U);
  EXPECT_FALSE(Refused.WorkLimitExceeded);
  EXPECT_EQ(print(*Stamped), Before);
}

TEST(SymSimplifyPredicates, DeepAffineGraphsFailClosedWithoutStackGrowth) {
  llvm::LLVMContext C;
  llvm::Module M("depth", C);
  auto *T = llvm::Type::getInt8Ty(C);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt1Ty(C), {T}, false),
      llvm::Function::ExternalLinkage, "f", M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  llvm::Value *V = F->getArg(0);
  for (unsigned I = 0; I != 8192; ++I)
    V = B.CreateAdd(V, B.getInt8(7));
  B.CreateRet(B.CreateICmpEQ(V, B.getInt8(13)));
  const std::string Before = print(*F);
  auto Opts = predicatesOnly();
  Opts.MaxPredicateWork = std::numeric_limits<size_t>::max();
  EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyPredicates, WorkIsCumulativeAcrossRootsAndSharedUseLists) {
  auto Make = [](llvm::Module &M, unsigned N, unsigned Uses) {
    auto &C = M.getContext();
    auto *T = llvm::Type::getInt8Ty(C);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(C), {T}, false),
        llvm::Function::ExternalLinkage, "f", M);
    auto *Sink = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(C),
                                {llvm::Type::getInt1Ty(C)}, false),
        llvm::Function::ExternalLinkage, "observe", M);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
    for (unsigned I = 0; I != N; ++I) {
      auto *A = B.CreateAdd(F->getArg(0), B.getInt8(127));
      auto *Bits = B.CreateOr(F->getArg(0), A);
      auto *R = B.CreateICmpSLT(Bits, B.getInt8(0));
      for (unsigned U = 0; U != Uses; ++U)
        B.CreateCall(Sink, {R});
    }
    B.CreateRetVoid();
    return F;
  };
  llvm::LLVMContext C;
  llvm::Module M("cumulative", C);
  auto *F = Make(M, 32, 1);
  llvm::ValueToValueMapTy Map;
  auto *Copy = llvm::CloneFunction(F, Map);
  auto Opts = predicatesOnly();
  Opts.MaxPredicateWork = 512;
  const unsigned Partial = SymSimplifyPass::simplify(*F, Opts);
  EXPECT_GT(Partial, 0u);
  EXPECT_LT(Partial, 32u);
  EXPECT_EQ(SymSimplifyPass::simplify(*Copy, predicatesOnly()), 32u);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));

  llvm::Module Shared("fan-out", C);
  F = Make(Shared, 1, 1024);
  const auto Before = print(*F);
  // Scanning the body fits, but counting all uses must consume more work.
  Opts.MaxPredicateWork = 1200;
  EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyPredicates, ALoopPhiRemainsTheOriginalRetainedValue) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i1 @f(i8 %x, i1 %again) {
entry:
  br label %loop
loop:
  %p = phi i8 [ %x, %entry ], [ %next, %loop ]
  %next = add i8 %p, 1
  %a = add i8 %p, 127
  %b = or i8 %p, %a
  %r = icmp slt i8 %b, 0
  br i1 %again, label %loop, label %exit
exit:
  ret i1 %r
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  EXPECT_GT(SymSimplifyPass::simplify(*F, predicatesOnly()), 0u);
  EXPECT_NE(print(*F).find("icmp ne i8 %p, 0"), std::string::npos);
  EXPECT_NE(print(*F).find("phi i8 [ %x, %entry ], [ %next, %loop ]"),
            std::string::npos);
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
}

TEST(SymSimplifyPredicates, RuntimeMatchesIndependentOracleAtO0AndO2) {
  llvm::LLVMContext C;
  llvm::Module M("predicate-runtime", C);
  for (unsigned Width : {8u, 64u})
    for (unsigned Kind = 0; Kind != 8; ++Kind) {
      const auto Suffix = std::to_string(Width) + "_" + std::to_string(Kind);
      auto *F = encoded(M, Width, "original" + Suffix, Kind);
      llvm::ValueToValueMapTy Map;
      auto *Copy = llvm::CloneFunction(F, Map);
      Copy->setName("simplified" + Suffix);
      ASSERT_GT(SymSimplifyPass::simplify(*Copy, predicatesOnly()), 0u);
    }
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program));
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> IR, Source, Binary, Error;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-predicates", "ll", IR));
  llvm::FileRemover RemoveIR(IR);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-predicates", "c", Source));
  llvm::FileRemover RemoveSource(Source);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-predicates", "exe", Binary));
  llvm::FileRemover RemoveBinary(Binary);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-predicates", "err", Error));
  llvm::FileRemover RemoveError(Error);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(IR, EC);
    ASSERT_FALSE(EC);
    M.print(OS, nullptr);
  }
  {
    llvm::raw_fd_ostream OS(Source, EC);
    ASSERT_FALSE(EC);
    OS << "#include <stdint.h>\n";
    for (unsigned Width : {8u, 64u})
      for (unsigned Kind = 0; Kind != 8; ++Kind)
        OS << "extern uint32_t original" << Width << "_" << Kind << "(uint"
           << Width << "_t), simplified" << Width << "_" << Kind << "(uint"
           << Width << "_t);\n";
    auto Checks = [&](unsigned Width) {
      for (unsigned Kind = 0; Kind != 8; ++Kind) {
        const auto Suffix = std::to_string(Width) + "_" + std::to_string(Kind);
        OS << "{ uint" << Width
           << "_t a = " << (Kind & 2 ? "UINT64_C(0) - x" : "x") << ";\n"
           << "a += 9; unsigned expected = (a != (uint" << Width << "_t)"
           << (Kind & 1 ? "-1" : "0") << ") ^ "
           << ((Kind & 1 ? 1 : 0) ^ (Kind & 4 ? 1 : 0)) << ";\n"
           << "if (original" << Suffix << "(x) != expected || simplified"
           << Suffix << "(x) != expected) return " << Kind + 1 << "; }\n";
      }
    };
    OS << "int main(void) {\nfor (unsigned x = 0; x != 256; ++x) {\n";
    Checks(8);
    OS << R"(
  }
  uint64_t r = UINT64_C(0xc83a94701d5e62fb);
  const uint64_t edges[] = {0, 1, 8, 9, 10, UINT64_MAX, UINT64_MAX-8,
      UINT64_MAX-9, UINT64_C(1)<<63, (UINT64_C(1)<<63)-1};
  for (unsigned k = 0; k != 8192; ++k) {
    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
    uint64_t x = k < sizeof(edges)/sizeof(edges[0]) ? edges[k] : r;
)";
    Checks(64);
    OS << "}\nreturn 0;\n}\n";
  }
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Error.str()};
    const int Exit = llvm::sys::ExecuteAndWait(
        Compiler, {Compiler, Optimization, IR, Source, "-o", Binary},
        std::nullopt, Redirects, 30);
    auto Errors = llvm::MemoryBuffer::getFile(Error);
    ASSERT_EQ(Exit, 0) << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Binary, {Binary}, std::nullopt,
                                        Redirects, 30),
              0);
  }
}

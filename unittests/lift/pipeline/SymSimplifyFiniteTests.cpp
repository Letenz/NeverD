//===- SymSimplifyFiniteTests.cpp - Complete two-valued slices
//-------------===//
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
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/ValueSymbolTable.h"
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

SymSimplifyOptions finiteOnly() {
  SymSimplifyOptions Opts;
  Opts.MinMeasuredNodes = std::numeric_limits<size_t>::max();
  Opts.MaxPredicateWork = 0;
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

llvm::Function *encoded(llvm::Module &M, unsigned Width, llvm::StringRef Name,
                        unsigned Seed = 0, bool ArithmeticResult = false) {
  auto &C = M.getContext();
  auto *T = llvm::IntegerType::get(C, Width);
  auto *FT = llvm::FunctionType::get(llvm::Type::getInt32Ty(C), {T}, false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  auto K = [&](uint64_t V) { return llvm::ConstantInt::get(T, V); };
  llvm::Value *V = F->getArg(0);
  switch (Seed) {
  case 0:
    V = B.CreateAnd(V, K(4), "anchor");
    V = B.CreateLShr(V, K(2), "", true);
    break;
  case 1:
    V = B.CreateAnd(K(4), V, "anchor");
    V = B.CreateLShr(V, K(2), "", true);
    break;
  case 2:
    V = B.CreateOr(V, llvm::ConstantInt::get(T, ~llvm::APInt(Width, 4)),
                   "anchor");
    V = B.CreateAnd(B.CreateNot(V), K(4));
    V = B.CreateLShr(V, K(2), "", true);
    break;
  case 3:
    V = B.CreateLShr(V, K(Width - 1), "anchor");
    break;
  case 4:
    V = B.CreateAShr(V, K(Width - 1), "anchor");
    V = B.CreateAnd(V, K(1));
    break;
  }
  V = B.CreateNSWSub(K(3), V);
  V = B.CreateMul(V, K(5), "", true, true);
  V = B.CreateXor(V, K(9));
  if (!ArithmeticResult)
    V = B.CreateICmpEQ(V, K(3));
  B.CreateRet(B.CreateZExtOrTrunc(V, B.getInt32Ty()));
  return F;
}

llvm::Function *nested(llvm::Module &M, unsigned Width, llvm::StringRef Name,
                       unsigned Kind = 0) {
  auto &C = M.getContext();
  auto *T = llvm::IntegerType::get(C, Width);
  auto *FT = llvm::FunctionType::get(llvm::Type::getInt32Ty(C), {T, T}, false);
  auto *F =
      llvm::Function::Create(FT, llvm::Function::ExternalLinkage, Name, M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  auto K = [&](uint64_t V) { return llvm::ConstantInt::get(T, V); };
  auto *Masked = B.CreateAnd(F->getArg(0), K(4));
  llvm::Value *Other = F->getArg(1);
  if (Kind == 3)
    Other = llvm::UndefValue::get(T);
  llvm::Value *Anchor = Kind == 2   ? B.CreateOr(Masked, Other, "anchor")
                        : Kind == 1 ? B.CreateAnd(Other, Masked, "anchor")
                                    : B.CreateAnd(Masked, Other, "anchor");
  auto *V = B.CreateLShr(Anchor, K(2), "", true);
  V = B.CreateNSWSub(K(3), V);
  V = B.CreateMul(V, K(5), "", true, true);
  V = B.CreateXor(V, K(9));
  V = B.CreateICmpEQ(V, K(3));
  B.CreateRet(B.CreateZExt(V, B.getInt32Ty()));
  return F;
}

llvm::Value *mergeResult(llvm::IRBuilder<> &B, llvm::Value *Anchor) {
  auto *T = llvm::cast<llvm::IntegerType>(Anchor->getType());
  auto K = [&](unsigned N) { return llvm::ConstantInt::get(T, N); };
  auto *V = B.CreateNUWAdd(Anchor, K(7));
  V = B.CreateXor(V, K(5));
  V = B.CreateMul(V, K(3), "", true, true);
  return B.CreateZExt(B.CreateICmpEQ(V, K(45)), B.getInt32Ty());
}

llvm::Function *merged(llvm::Module &M, unsigned Width, llvm::StringRef Name,
                       unsigned Kind) {
  auto &C = M.getContext();
  auto *T = llvm::IntegerType::get(C, Width);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(C), {T, T}, false),
      llvm::Function::ExternalLinkage, Name, M);
  auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(Entry);
  auto K = [&](unsigned N) { return llvm::ConstantInt::get(T, N); };
  auto Bit = [&](llvm::Value *V, unsigned Mask) {
    return B.CreateICmpNE(B.CreateAnd(V, K(Mask)), K(0));
  };
  llvm::Value *Anchor = B.CreateSelect(Bit(F->getArg(0), 4), K(3), K(11));
  if (Kind == 0) {
    auto *Other = B.CreateSelect(Bit(F->getArg(0), 16), K(3), K(11));
    Anchor = B.CreateSelect(Bit(F->getArg(1), 2), Anchor, Other);
  } else if (Kind == 1) {
    auto *Left = llvm::BasicBlock::Create(C, "left", F);
    auto *Right = llvm::BasicBlock::Create(C, "right", F);
    auto *Join = llvm::BasicBlock::Create(C, "join", F);
    auto *Other = B.CreateSelect(Bit(F->getArg(0), 16), K(3), K(11));
    B.CreateCondBr(Bit(F->getArg(1), 2), Left, Right);
    B.SetInsertPoint(Left);
    B.CreateBr(Join);
    B.SetInsertPoint(Right);
    B.CreateBr(Join);
    B.SetInsertPoint(Join);
    auto *Phi = B.CreatePHI(T, 2);
    Phi->addIncoming(Anchor, Left);
    Phi->addIncoming(Other, Right);
    Anchor = Phi;
  } else if (Kind == 2) {
    auto *Limit = B.CreateAnd(F->getArg(1), K(3));
    auto *Keep = Bit(F->getArg(1), 4);
    auto *Loop = llvm::BasicBlock::Create(C, "loop", F);
    auto *Latch = llvm::BasicBlock::Create(C, "latch", F);
    auto *Exit = llvm::BasicBlock::Create(C, "exit", F);
    B.CreateBr(Loop);
    B.SetInsertPoint(Loop);
    auto *Index = B.CreatePHI(T, 2);
    auto *Phi = B.CreatePHI(T, 2);
    Index->addIncoming(K(0), Entry);
    Phi->addIncoming(Anchor, Entry);
    B.CreateCondBr(B.CreateICmpEQ(Index, Limit), Exit, Latch);
    B.SetInsertPoint(Latch);
    auto *Flip = B.CreateSelect(B.CreateICmpEQ(Phi, K(3)), K(11), K(3));
    auto *Next = B.CreateSelect(Keep, Phi, Flip, "backedge");
    Index->addIncoming(B.CreateAdd(Index, K(1)), Latch);
    Phi->addIncoming(Next, Latch);
    B.CreateBr(Loop);
    B.SetInsertPoint(Exit);
    Anchor = Phi;
  }
  Anchor->setName("anchor");
  B.CreateRet(mergeResult(B, Anchor));
  return F;
}

llvm::Function *phiChain(llvm::Module &M, unsigned Length) {
  auto &C = M.getContext();
  auto *T = llvm::Type::getInt32Ty(C);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(T, {llvm::Type::getInt1Ty(C)}, false),
      llvm::Function::ExternalLinkage, "f", M);
  auto *Previous = llvm::BasicBlock::Create(C, "entry", F);
  llvm::IRBuilder<> B(Previous);
  llvm::Value *V = B.CreateSelect(F->getArg(0), B.getInt32(3), B.getInt32(11));
  for (unsigned N = 0; N != Length; ++N) {
    auto *Next = llvm::BasicBlock::Create(C, "next", F);
    B.CreateBr(Next);
    B.SetInsertPoint(Next);
    auto *Phi = B.CreatePHI(T, 1);
    Phi->addIncoming(V, Previous);
    V = Phi;
    Previous = Next;
  }
  B.CreateRet(mergeResult(B, V));
  return F;
}

std::unique_ptr<llvm::Module> parse(llvm::LLVMContext &C,
                                    llvm::StringRef Body) {
  llvm::SMDiagnostic E;
  auto M = llvm::parseAssemblyString(Body, E, C);
  if (!M) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    E.print("finite-slice-test", OS);
    ADD_FAILURE() << Text;
  }
  return M;
}

} // namespace

TEST(SymSimplifyFinite, RecoversConditionsAndValuesAtEveryIntegerWidth) {
  for (unsigned Width : {8u, 16u, 32u, 64u, 128u, 512u})
    for (unsigned Seed = 0; Seed != 5; ++Seed)
      for (bool Arithmetic : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ":" << Seed << ":" << Arithmetic);
        llvm::LLVMContext C;
        llvm::Module M("finite", C);
        auto *F = encoded(M, Width, "f", Seed, Arithmetic);
        const unsigned Before = count(*F);
        EXPECT_GT(SymSimplifyPass::simplify(*F, finiteOnly()), 0u) << print(*F);
        EXPECT_LT(count(*F), Before);
        EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
        EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u) << print(*F);
      }
}

TEST(SymSimplifyFinite, NestedConjunctionRetainsItsCompleteAnchor) {
  for (unsigned Width : {8U, 32U, 64U, 128U, 512U})
    for (unsigned Kind : {0U, 1U}) {
      SCOPED_TRACE(::testing::Message() << Width << ":" << Kind);
      llvm::LLVMContext C;
      llvm::Module M("nested", C);
      auto *F = nested(M, Width, "f", Kind);
      auto *Anchor = llvm::cast<llvm::Instruction>(
          F->getValueSymbolTable()->lookup("anchor"));
      llvm::ValueToValueMapTy Map;
      auto *Copy = llvm::CloneFunction(F, Map);
      auto R = SymSimplifyPass::simplifyFiniteValues(*F);
      ASSERT_GT(R.Rewrites, 0U) << print(*F);
      EXPECT_GT(R.Work, 0U);
      EXPECT_FALSE(R.WorkLimitExceeded);
      EXPECT_FALSE(Anchor->use_empty());
      EXPECT_EQ(Anchor->getOpcode(), llvm::Instruction::And);
      EXPECT_GT(SymSimplifyPass::simplify(*Copy, finiteOnly()), 0U);
      EXPECT_EQ(count(*F), count(*Copy));
      EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
    }
}

TEST(SymSimplifyFinite, ConjunctionDiscoveryCannotCrossOrOrUndef) {
  for (unsigned Kind : {2U, 3U}) {
    llvm::LLVMContext C;
    llvm::Module M("refused", C);
    auto *F = nested(M, 64, "f", Kind);
    auto Before = print(*F);
    auto R = SymSimplifyPass::simplifyFiniteValues(*F);
    EXPECT_EQ(R.Rewrites, 0U);
    EXPECT_EQ(print(*F), Before);
  }
}

TEST(SymSimplifyFinite, DeepConjunctionDiscoveryRemainsBounded) {
  llvm::LLVMContext C;
  llvm::Module M("deep-conjunction", C);
  auto *F = nested(M, 64, "f");
  auto *Anchor =
      llvm::cast<llvm::Instruction>(F->getValueSymbolTable()->lookup("anchor"));
  llvm::IRBuilder<> B(Anchor);
  llvm::Value *Deep = Anchor->getOperand(0);
  for (unsigned N = 0; N < 12; ++N)
    Deep = B.CreateAnd(Deep, F->getArg(1));
  Anchor->setOperand(0, Deep);
  auto Before = print(*F);
  auto R = SymSimplifyPass::simplifyFiniteValues(*F);
  EXPECT_EQ(R.Rewrites, 0U);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyFinite, StandaloneWorkIsBoundedAndStampAware) {
  auto Attempt = [](size_t Budget, bool Stamped = false) {
    llvm::LLVMContext C;
    llvm::Module M("nested-budget", C);
    auto *F = nested(M, 64, "f");
    if (Stamped)
      F->addFnAttr(kObfuscatedFnAttr);
    auto Before = print(*F);
    auto Opts = finiteOnly();
    Opts.MaxFiniteValueWork = Budget;
    auto R = SymSimplifyPass::simplifyFiniteValues(*F, Opts);
    EXPECT_LE(R.Work, Budget);
    if (!R.Rewrites)
      EXPECT_EQ(print(*F), Before);
    EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
    return R;
  };
  const auto Baseline = Attempt(65536);
  ASSERT_GT(Baseline.Rewrites, 0U);
  EXPECT_FALSE(Baseline.WorkLimitExceeded);
  EXPECT_EQ(Attempt(0).Work, 0U);
  EXPECT_EQ(Attempt(65536, true).Work, 0U);
  size_t Low = 0, High = Baseline.Work;
  ASSERT_GT(Attempt(High).Rewrites, 0U);
  while (High - Low > 1) {
    const auto Middle = Low + (High - Low) / 2;
    if (Attempt(Middle).Rewrites)
      High = Middle;
    else
      Low = Middle;
  }
  const auto Short = Attempt(High - 1);
  EXPECT_EQ(Short.Rewrites, 0U);
  EXPECT_TRUE(Short.WorkLimitExceeded);
  EXPECT_GT(Attempt(High).Rewrites, 0U);
}

TEST(SymSimplifyFinite, CompleteMergeDomainsRetainTheOriginalObservation) {
  for (unsigned Width : {8U, 32U, 64U, 128U, 512U})
    for (unsigned Kind : {0U, 1U, 2U}) {
      SCOPED_TRACE(::testing::Message() << Width << ":" << Kind);
      llvm::LLVMContext C;
      llvm::Module M("merges", C);
      auto *F = merged(M, Width, "f", Kind);
      auto *Anchor = llvm::cast<llvm::Instruction>(
          F->getValueSymbolTable()->lookup("anchor"));
      llvm::WeakTrackingVH Observed(Anchor);
      llvm::ValueToValueMapTy Map;
      auto *Copy = llvm::CloneFunction(F, Map);
      auto R = SymSimplifyPass::simplifyFiniteValues(*F);
      EXPECT_GT(R.Rewrites, 0U) << print(*F);
      EXPECT_FALSE(R.WorkLimitExceeded);
      auto *Retained = llvm::dyn_cast_or_null<llvm::Instruction>(Observed);
      ASSERT_TRUE(Retained);
      EXPECT_FALSE(Retained->use_empty());
      EXPECT_GT(SymSimplifyPass::simplify(*Copy, finiteOnly()), 0U);
      EXPECT_EQ(count(*F), count(*Copy));
      EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
    }
}

TEST(SymSimplifyFinite, MergeDomainsRefuseConflictingAndUndefinedInputs) {
  for (unsigned Kind : {0U, 1U, 2U, 3U, 4U}) {
    llvm::LLVMContext C;
    llvm::Module M("conflict", C);
    auto *F = merged(M, 32, "f", Kind == 4 ? 2 : 0);
    auto *Anchor = llvm::cast<llvm::Instruction>(
        F->getValueSymbolTable()->lookup("anchor"));
    if (Kind == 4) {
      auto *Back = llvm::cast<llvm::SelectInst>(
          F->getValueSymbolTable()->lookup("backedge"));
      Back->setFalseValue(F->getArg(0));
    } else {
      auto *Select = llvm::cast<llvm::SelectInst>(Anchor);
      if (Kind == 0)
        Select->setFalseValue(F->getArg(1));
      if (Kind == 1)
        Select->setFalseValue(llvm::UndefValue::get(Select->getType()));
      if (Kind == 2)
        Select->setCondition(
            llvm::PoisonValue::get(Select->getCondition()->getType()));
      if (Kind == 3) {
        llvm::IRBuilder<> B(Select);
        auto *Extra =
            B.CreateSelect(B.CreateICmpNE(F->getArg(1), B.getInt32(0)),
                           B.getInt32(11), B.getInt32(17));
        Select->setFalseValue(Extra);
      }
    }
    ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
    auto Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplifyFiniteValues(*F).Rewrites, 0U)
        << Kind << "\n"
        << print(*F);
    EXPECT_EQ(print(*F), Before) << Kind;
  }
}

TEST(SymSimplifyFinite, MergeDomainCannotBorrowASeedForAnUnanchoredCycle) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i32 @f(i1 %pick, i1 %leave) {
    entry: ret i32 0
    cycle:
      %unanchored = phi i32 [ %unanchored, %cycle ]
      br i1 %leave, label %exit, label %cycle
    exit:
      %seed = select i1 %pick, i32 3, i32 11
      %anchor = select i1 %pick, i32 %unanchored, i32 %seed
      %a = add i32 %anchor, 7
      %b = xor i32 %a, 5
      %c = mul i32 %b, 3
      %d = icmp eq i32 %c, 45
      %e = zext i1 %d to i32
      ret i32 %e
    })");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
  auto Before = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplifyFiniteValues(*F).Rewrites, 0U);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyFinite, MergeDomainsHaveBoundedGraphAndWork) {
  for (unsigned Length : {29U, 30U}) {
    llvm::LLVMContext C;
    llvm::Module M("chain", C);
    auto *F = phiChain(M, Length);
    auto R = SymSimplifyPass::simplifyFiniteValues(*F);
    EXPECT_EQ(R.Rewrites != 0, Length == 29) << print(*F);
    EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
  }
  auto Run = [&](size_t Work) {
    llvm::LLVMContext C;
    llvm::Module M("work", C);
    auto *F = merged(M, 64, "f", 2);
    auto O = finiteOnly();
    O.MaxFiniteValueWork = Work;
    auto R = SymSimplifyPass::simplifyFiniteValues(*F, O);
    EXPECT_LE(R.Work, Work);
    EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
    return R;
  };
  size_t Low = 1, High = 65536;
  ASSERT_GT(Run(High).Rewrites, 0U);
  while (Low < High) {
    auto Mid = Low + (High - Low) / 2;
    if (Run(Mid).Rewrites)
      High = Mid;
    else
      Low = Mid + 1;
  }
  EXPECT_GT(Run(Low).Rewrites, 0U);
  EXPECT_EQ(Run(Low - 1).Rewrites, 0U);
  EXPECT_TRUE(Run(Low - 1).WorkLimitExceeded);
}

TEST(SymSimplifyFinite, MergeDomainsKeepDistinctPhiAndFreezeIdentities) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i32 @f(i1 %path) {
    entry:
      %first = freeze i1 undef
      %second = freeze i1 undef
      %a = select i1 %first, i32 3, i32 11
      %b = select i1 %second, i32 3, i32 11
      br i1 %path, label %left, label %right
    left: br label %join
    right: br label %join
    join:
      %x = phi i32 [ %a, %left ], [ %a, %right ]
      %y = phi i32 [ %b, %left ], [ %b, %right ]
      %difference = sub i32 %x, %y
      %test = icmp ne i32 %difference, 0
      %result = zext i1 %test to i32
      ret i32 %result
    })");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
  auto Before = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplifyFiniteValues(*F).Rewrites, 0U);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyFinite, MergeDomainRetainsFlaggedProducerDependence) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
    define i32 @f(i8 %x, i1 %choose) {
      %flagged = add nsw i8 %x, 100
      %masked = and i8 %flagged, 4
      %anchor = select i1 %choose, i8 %masked, i8 4
      %bit = lshr exact i8 %anchor, 2
      %a = sub nsw i8 3, %bit
      %b = mul nuw nsw i8 %a, 5
      %c = xor i8 %b, 9
      %d = icmp eq i8 %c, 3
      %e = zext i1 %d to i32
      ret i32 %e
    })");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  llvm::WeakTrackingVH Producer(F->getValueSymbolTable()->lookup("flagged"));
  llvm::WeakTrackingVH Anchor(F->getValueSymbolTable()->lookup("anchor"));
  ASSERT_GT(SymSimplifyPass::simplifyFiniteValues(*F).Rewrites, 0U);
  auto *Retained = llvm::dyn_cast_or_null<llvm::BinaryOperator>(Producer);
  ASSERT_TRUE(Retained);
  EXPECT_TRUE(Retained->hasNoSignedWrap());
  EXPECT_FALSE(Retained->use_empty());
  ASSERT_TRUE(llvm::dyn_cast_or_null<llvm::SelectInst>(Anchor));
  EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
}

TEST(SymSimplifyFinite, MergeDomainCountsEveryIncomingEdge) {
  for (unsigned Incoming : {64U, 65U}) {
    llvm::LLVMContext C;
    llvm::Module M("fanout", C);
    auto *T = llvm::Type::getInt32Ty(C);
    auto *F = llvm::Function::Create(llvm::FunctionType::get(T, {T}, false),
                                     llvm::Function::ExternalLinkage, "f", M);
    auto *Entry = llvm::BasicBlock::Create(C, "entry", F);
    auto *Join = llvm::BasicBlock::Create(C, "join", F);
    llvm::IRBuilder<> B(Join);
    auto *Phi = B.CreatePHI(T, Incoming);
    B.CreateRet(mergeResult(B, Phi));
    llvm::SmallVector<llvm::BasicBlock *, 8> Predecessors;
    for (unsigned N = 0; N != Incoming; ++N) {
      auto *Block = llvm::BasicBlock::Create(C, "case", F, Join);
      Predecessors.push_back(Block);
      B.SetInsertPoint(Block);
      B.CreateBr(Join);
      Phi->addIncoming(B.getInt32(N & 1 ? 3 : 11), Block);
    }
    B.SetInsertPoint(Entry);
    auto *Switch =
        B.CreateSwitch(F->getArg(0), Predecessors.back(), Incoming - 1);
    for (unsigned N = 0; N != Incoming - 1; ++N)
      Switch->addCase(B.getInt32(N), Predecessors[N]);
    ASSERT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
    auto R = SymSimplifyPass::simplifyFiniteValues(*F);
    EXPECT_EQ(R.Rewrites != 0, Incoming == 64) << print(*F);
    EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
  }
}

TEST(SymSimplifyFinite, DoesNotSpendTheSameSavingsTwiceForSharedUses) {
  llvm::LLVMContext C;
  llvm::Module M("shared", C);
  auto *F = encoded(M, 64, "f");
  auto *Ret = llvm::cast<llvm::ReturnInst>(F->getEntryBlock().getTerminator());
  auto *Sink = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(C),
                              {llvm::Type::getInt64Ty(C)}, false),
      llvm::Function::ExternalLinkage, "observe", M);
  llvm::SmallVector<llvm::Instruction *, 8> Shared;
  for (auto &I : F->getEntryBlock())
    if (I.getType()->isIntegerTy(64))
      Shared.push_back(&I);
  llvm::IRBuilder<> B(Ret);
  for (auto *I : Shared)
    B.CreateCall(Sink, {I});
  const unsigned Before = count(*F);
  SymSimplifyPass::simplify(*F, finiteOnly());
  EXPECT_LE(count(*F), Before);
  unsigned Calls = 0;
  for (const auto &I : llvm::instructions(F))
    Calls += llvm::isa<llvm::CallInst>(I);
  EXPECT_EQ(Calls, Shared.size());
  EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
}

TEST(SymSimplifyFinite, RefusesAnyUndefinedEnumeratedOperation) {
  const char *Bodies[] = {
      "%a = and i8 %x, 4\n%b = add nuw i8 %a, -1",
      "%a = and i8 %x, 4\n%b = mul nsw i8 %a, 32",
      "%a = and i8 %x, 4\n%b = shl nuw i8 %a, 6",
      "%a = and i8 %x, 4\n%b = lshr exact i8 %a, 3",
      "%a = and i8 %x, 4\n%b = ashr exact i8 %a, 3",
      "%a = and i8 %x, 4\n%b = or disjoint i8 %a, 4",
      "%a = and i8 %x, 8\n%b = shl i8 3, %a",
      "%a = and i8 %x, -128\n%w = zext nneg i8 %a to i16\n%b = trunc i16 %w to "
      "i8",
      "%a = and i8 %x, 1\n%w = zext i8 %a to i16\n%v = shl i16 %w, 8\n%b = "
      "trunc nuw i16 %v to i8",
      "%a = and i8 %x, 1\n%w = zext i8 %a to i16\n%v = shl i16 %w, 7\n%b = "
      "trunc nsw i16 %v to i8",
  };
  for (const char *Body : Bodies) {
    SCOPED_TRACE(Body);
    llvm::LLVMContext C;
    auto M = parse(C, "define i1 @f(i8 %x) {\n" + std::string(Body) +
                          "\n%c = mul i8 %b, 11\n%d = xor i8 %c, 23\n"
                          "%r = icmp eq i8 %d, 9\nret i1 %r\n}");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
}

TEST(SymSimplifyFinite, KeepsSameSignPoisonAndConstantDependence) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i32 @f(i8 %x) {
  %a = and i8 %x, 4
  %bad = icmp samesign eq i8 %a, -1
  %s = select i1 %bad, i32 23, i32 91
  %t = xor i32 %s, 11
  %r = add i32 %t, 7
  ret i32 %r
}
define i32 @constant(i1 %b) {
  %s = select i1 %b, i32 32, i32 64
  %t = and i32 %s, 7
  ret i32 %t
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  SymSimplifyPass::simplify(*F, finiteOnly());
  EXPECT_NE(print(*F).find("icmp samesign"), std::string::npos);
  auto *K = M->getFunction("constant");
  const std::string Before = print(*K);
  EXPECT_EQ(SymSimplifyPass::simplify(*K, finiteOnly()), 0u);
  EXPECT_EQ(print(*K), Before);
}

TEST(SymSimplifyFinite, DoesNotCorrelateDifferentInputsOrVolatileReads) {
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i1 @f(ptr %p) {
  %x = load volatile i32, ptr %p
  %y = load volatile i32, ptr %p
  %a = and i32 %x, 4
  %b = and i32 %y, 4
  %c = xor i32 %a, %b
  %d = mul i32 %c, 11
  %r = icmp eq i32 %d, 0
  ret i1 %r
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  const std::string Before = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyFinite, ExplicitUndefinedAndIndependentFreezesStayOpaque) {
  for (llvm::StringRef Input : {"undef", "poison"}) {
    llvm::LLVMContext C;
    auto M = parse(C, "define i1 @f() {\n%a = and i32 " + Input.str() +
                          ", 4\n%b = mul i32 %a, 11\n%c = xor i32 %b, 23\n"
                          "%r = icmp eq i32 %c, 9\nret i1 %r\n}");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
  llvm::LLVMContext C;
  auto M = parse(C, R"(
define i1 @f() {
  %x = freeze i1 undef
  %y = freeze i1 undef
  %a = select i1 %x, i32 19, i32 43
  %b = select i1 %y, i32 19, i32 43
  %c = xor i32 %a, %b
  %r = icmp eq i32 %c, 0
  ret i1 %r
})");
  ASSERT_TRUE(M);
  auto *F = M->getFunction("f");
  const std::string Before = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u);
  EXPECT_EQ(print(*F), Before);
}

TEST(SymSimplifyFinite, IterativeWalkHandlesDeepExpressionsWithoutSampling) {
  llvm::LLVMContext C;
  llvm::Module M("deep", C);
  auto *T = llvm::Type::getInt32Ty(C);
  auto *F = llvm::Function::Create(llvm::FunctionType::get(T, {T}, false),
                                   llvm::Function::ExternalLinkage, "f", M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(C, "entry", F));
  llvm::Value *V = B.CreateAnd(F->getArg(0), B.getInt32(1));
  for (unsigned I = 0; I != 2048; ++I) {
    V = B.CreateAdd(V, B.getInt32(I + 3));
    V = B.CreateXor(V, B.getInt32(5 * I + 11));
  }
  B.CreateRet(V);
  auto Opts = finiteOnly();
  Opts.MaxFiniteValueWork = 1000000;
  EXPECT_GT(SymSimplifyPass::simplify(*F, Opts), 0u);
  EXPECT_LE(count(*F), 4u);
  EXPECT_FALSE(llvm::verifyFunction(*F, &llvm::errs()));
}

TEST(SymSimplifyFinite, DoesNotHideUndefinedInputsBehindPhiAnchors) {
  for (llvm::StringRef Input : {"undef", "poison"}) {
    llvm::LLVMContext C;
    auto M = parse(C, R"(
define i32 @f(i1 %c) {
entry:
  br i1 %c, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %p = phi i1 [ true, %left ], [ )" +
                          Input.str() + R"(, %right ]
  %s = select i1 %p, i32 19, i32 43
  %t = mul i32 %s, 13
  %v = xor i32 %t, 7
  ret i32 %v
})");
    ASSERT_TRUE(M);
    auto *F = M->getFunction("f");
    const std::string Before = print(*F);
    EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u);
    EXPECT_EQ(print(*F), Before);
  }
}

TEST(SymSimplifyFinite, BudgetsSavingsAndObfuscationStampFailClosed) {
  for (size_t Budget : {size_t(0), size_t(1), size_t(8)}) {
    llvm::LLVMContext C;
    llvm::Module M("budget", C);
    auto *F = encoded(M, 64, "f");
    const std::string Before = print(*F);
    auto Opts = finiteOnly();
    Opts.MaxFiniteValueWork = Budget;
    EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
    EXPECT_EQ(print(*F), Before);
  }
  llvm::LLVMContext C;
  llvm::Module M("policy", C);
  auto *F = encoded(M, 64, "f");
  const std::string Before = print(*F);
  auto Opts = finiteOnly();
  Opts.MinInstructionsSaved = std::numeric_limits<size_t>::max();
  EXPECT_EQ(SymSimplifyPass::simplify(*F, Opts), 0u);
  EXPECT_EQ(print(*F), Before);
  F->addFnAttr(kObfuscatedFnAttr);
  const std::string Stamped = print(*F);
  EXPECT_EQ(SymSimplifyPass::simplify(*F, finiteOnly()), 0u);
  EXPECT_EQ(print(*F), Stamped);
}

TEST(SymSimplifyFinite, RuntimeChecksBothValuesAndAllUnrelatedInputBits) {
  llvm::LLVMContext C;
  llvm::Module M("finite-runtime", C);
  for (unsigned Width : {8u, 64u})
    for (unsigned Seed = 0; Seed != 5; ++Seed) {
      const std::string Suffix =
          std::to_string(Width) + "_" + std::to_string(Seed);
      auto *F = encoded(M, Width, "original" + Suffix, Seed);
      llvm::ValueToValueMapTy Map;
      auto *Copy = llvm::CloneFunction(F, Map);
      Copy->setName("simplified" + Suffix);
      ASSERT_GT(SymSimplifyPass::simplify(*Copy, finiteOnly()), 0u);
    }
  for (unsigned Width : {8U, 64U}) {
    auto *F = nested(M, Width, "nested_original" + std::to_string(Width));
    llvm::ValueToValueMapTy Map;
    auto *Copy = llvm::CloneFunction(F, Map);
    Copy->setName("nested_simplified" + std::to_string(Width));
    ASSERT_GT(SymSimplifyPass::simplifyFiniteValues(*Copy).Rewrites, 0U);
  }
  for (unsigned Width : {8U, 64U})
    for (unsigned Kind : {0U, 1U, 2U}) {
      auto Suffix = std::to_string(Width) + "_" + std::to_string(Kind);
      auto *F = merged(M, Width, "merged_original" + Suffix, Kind);
      llvm::ValueToValueMapTy Map;
      auto *Copy = llvm::CloneFunction(F, Map);
      Copy->setName("merged_simplified" + Suffix);
      ASSERT_GT(SymSimplifyPass::simplifyFiniteValues(*Copy).Rewrites, 0U);
    }
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program));
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> IR, Source, Binary, Error;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-finite", "ll", IR));
  llvm::FileRemover RemoveIR(IR);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-finite", "c", Source));
  llvm::FileRemover RemoveSource(Source);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-finite", "exe", Binary));
  llvm::FileRemover RemoveBinary(Binary);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-finite", "err", Error));
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
      for (unsigned Seed = 0; Seed != 5; ++Seed)
        OS << "extern uint32_t original" << Width << "_" << Seed << "(uint"
           << Width << "_t), simplified" << Width << "_" << Seed << "(uint"
           << Width << "_t);\n";
    for (unsigned Width : {8U, 64U})
      OS << "extern uint32_t nested_original" << Width << "(uint" << Width
         << "_t, uint" << Width << "_t), nested_simplified" << Width << "(uint"
         << Width << "_t, uint" << Width << "_t);\n";
    for (unsigned Width : {8U, 64U})
      for (unsigned Kind : {0U, 1U, 2U})
        OS << "extern uint32_t merged_original" << Width << "_" << Kind
           << "(uint" << Width << "_t, uint" << Width
           << "_t), merged_simplified" << Width << "_" << Kind << "(uint"
           << Width << "_t, uint" << Width << "_t);\n";
    auto Checks = [&](unsigned Width) {
      for (unsigned Seed = 0; Seed != 5; ++Seed) {
        const std::string Suffix =
            std::to_string(Width) + "_" + std::to_string(Seed);
        const unsigned Shift = Seed < 3 ? 2 : Width - 1;
        OS << "{ unsigned answer = ((x >> " << Shift << ") & 1) ^ "
           << (Seed == 2 ? 1 : 0) << ";\n"
           << "if (original" << Suffix << "(x) != answer || simplified"
           << Suffix << "(x) != answer) return " << Seed + 1 << "; }\n";
      }
    };
    OS << "int main(void) {\nfor (unsigned x = 0; x != 256; ++x) {\n";
    Checks(8);
    OS << R"(
  }
  uint64_t r = UINT64_C(0x32c543efab192617);
  for (unsigned k = 0; k != 8192; ++k) {
    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
    uint64_t x = k == 0 ? 0 : k == 1 ? UINT64_MAX : r;
)";
    Checks(64);
    OS << R"(
  }
  for (unsigned k = 0; k < 65536u + 4096u; ++k) {
    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
    uint64_t x = k < 65536u ? (r & ~UINT64_C(255)) | (k & 255u) : r;
    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
    uint64_t y = k < 65536u ? (r & ~UINT64_C(255)) | (k >> 8) : r;
    unsigned answer = (x & y & 4u) != 0;
    if (nested_original8(x, y) != answer || nested_simplified8(x, y) != answer ||
        nested_original64(x, y) != answer || nested_simplified64(x, y) != answer)
      return 6;
    unsigned seed_answer = (x & 4u) != 0;
    unsigned select_answer = (y & 2u) ? seed_answer : ((x & 16u) != 0);
    unsigned phi_answer = select_answer;
    unsigned loop_answer = seed_answer ^ ((!(y & 4u)) && (y & 1u));
    if (merged_original8_0(x,y) != select_answer || merged_simplified8_0(x,y) != select_answer) return 7;
    if (merged_original8_1(x,y) != phi_answer || merged_simplified8_1(x,y) != phi_answer) return 8;
    if (merged_original8_2(x,y) != loop_answer || merged_simplified8_2(x,y) != loop_answer) return 9;
    if (merged_original64_0(x,y) != select_answer || merged_simplified64_0(x,y) != select_answer) return 7;
    if (merged_original64_1(x,y) != phi_answer || merged_simplified64_1(x,y) != phi_answer) return 8;
    if (merged_original64_2(x,y) != loop_answer || merged_simplified64_2(x,y) != loop_answer) return 9;
  }
  return 0;
})";
  }
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Error.str()};
    int Exit = llvm::sys::ExecuteAndWait(
        Compiler, {Compiler, Optimization, IR, Source, "-o", Binary},
        std::nullopt, Redirects, 30);
    auto Errors = llvm::MemoryBuffer::getFile(Error);
    ASSERT_EQ(Exit, 0) << (Errors ? (*Errors)->getBuffer().str() : "");
    EXPECT_EQ(llvm::sys::ExecuteAndWait(Binary, {Binary}, std::nullopt,
                                        Redirects, 30),
              0);
  }
}

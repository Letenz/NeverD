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

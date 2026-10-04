//===- ByteCellScalarizationTest.h - Independent cell fixtures ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_BYTECELLSCALARIZATIONTEST_H
#define NEVERD_UNITTESTS_BYTECELLSCALARIZATIONTEST_H

#include "gtest/gtest.h"

#include "neverd/pass/ir/simplify/ByteCellScalarizationPass.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Scalar/SROA.h"

namespace neverd::cell_test {
inline std::string print(const llvm::Module &M) {
  std::string S;
  llvm::raw_string_ostream O(S);
  M.print(O, nullptr);
  return S;
}

inline void replace(std::string &Text, llvm::StringRef From,
                    llvm::StringRef To) {
  auto At = Text.find(From.str());
  ASSERT_NE(At, std::string::npos) << From.str();
  Text.replace(At, From.size(), To.str());
}

inline std::unique_ptr<llvm::Module>
parse(llvm::LLVMContext &C, llvm::StringRef Text,
      llvm::StringRef Layout = "e-p:64:64") {
  llvm::SMDiagnostic Error;
  auto M = llvm::parseAssemblyString(Text, Error, C);
  EXPECT_TRUE(M) << Error.getMessage().str();
  if (M) {
    M->setDataLayout(Layout);
    EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  }
  return M;
}

inline void promote(llvm::Function &F) {
  llvm::PassBuilder PB;
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  llvm::SROAPass(llvm::SROAOptions::PreserveCFG).run(F, FAM);
  EXPECT_FALSE(llvm::verifyFunction(F, &llvm::errs()));
}

template <typename T> unsigned count(const llvm::Function &F) {
  unsigned N = 0;
  for (auto &I : llvm::instructions(F))
    N += llvm::isa<T>(I);
  return N;
}

// Original synthetic byte algorithm: two entry arms, partial overlap across
// three words, and different stores on two loop backedges. No native profile.
inline std::string program() {
  return R"(
define i64 @f(i64 %x, i64 %y, i32 %n, ptr %out) {
entry:
  %bytes = alloca [24 x i8], align 8
  %p1 = getelementptr i8, ptr %bytes, i64 1
  %p3 = getelementptr i8, ptr %bytes, i64 3
  %p7 = getelementptr i8, ptr %bytes, i64 7
  %p8 = getelementptr i8, ptr %bytes, i64 8
  %p12 = getelementptr i8, ptr %bytes, i64 12
  %p14 = getelementptr i8, ptr %bytes, i64 14
  %p16 = getelementptr i8, ptr %bytes, i64 16
  store i64 %x, ptr %bytes, align 1
  store i64 %y, ptr %p8, align 1
  %seed = xor i64 %x, %y
  store i64 %seed, ptr %p16, align 1
  %low = trunc i64 %y to i16
  %bit = and i32 %n, 1
  %choose = icmp eq i32 %bit, 0
  br i1 %choose, label %left, label %right
left:
  store i16 %low, ptr %p1, align 1
  br label %loop
right:
  %other = trunc i64 %x to i32
  store i32 %other, ptr %p1, align 1
  br label %loop
loop:
  %i = phi i32 [ 0, %left ], [ 0, %right ], [ %next, %even ], [ %next, %odd ]
  %bound = and i32 %n, 15
  %more = icmp ult i32 %i, %bound
  br i1 %more, label %body, label %done
body:
  %old = load i64, ptr %p3, align 1
  %next = add i32 %i, 1
  %parity = and i32 %i, 1
  %evenBit = icmp eq i32 %parity, 0
  br i1 %evenBit, label %even, label %odd
even:
  %sum = add i64 %old, %x
  store i64 %sum, ptr %p7, align 1
  br label %loop
odd:
  %small = trunc i64 %old to i32
  %mix = xor i32 %small, %i
  store i32 %mix, ptr %p12, align 1
  br label %loop
done:
  %a = load i64, ptr %p3, align 1
  %b = load i64, ptr %p14, align 1
  %answer = xor i64 %a, %b
  %first = load i64, ptr %bytes, align 1
  %second = load i64, ptr %p8, align 1
  %third = load i64, ptr %p16, align 1
  %out8 = getelementptr i8, ptr %out, i64 8
  %out16 = getelementptr i8, ptr %out, i64 16
  store i64 %first, ptr %out, align 1
  store i64 %second, ptr %out8, align 1
  store i64 %third, ptr %out16, align 1
  ret i64 %answer
}
)";
}
} // namespace neverd::cell_test
#endif

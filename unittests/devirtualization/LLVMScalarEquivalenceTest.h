//===- LLVMScalarEquivalenceTest.h - Independent scalar source fixtures ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_LLVM_SCALAR_EQUIVALENCE_TEST_H
#define NEVERD_UNITTESTS_LLVM_SCALAR_EQUIVALENCE_TEST_H

#include "gtest/gtest.h"

#include "neverd/analysis/LLVMScalarEquivalence.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::analysis::scalar_test {
using Status = LLVMScalarEquivalenceStatus;

struct Source {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module;
  explicit Source(llvm::StringRef IR) {
    llvm::SMDiagnostic Error;
    Module = llvm::parseAssemblyString(IR, Error, Context);
    std::string Message;
    llvm::raw_string_ostream OS(Message);
    Error.print("scalar test", OS);
    EXPECT_TRUE(Module) << Message;
  }
  llvm::Function &function() { return *Module->getFunction("f"); }
  std::string text() {
    std::string S;
    llvm::raw_string_ostream OS(S);
    Module->print(OS, nullptr);
    return S;
  }
};

inline LLVMScalarEquivalenceResult
check(llvm::StringRef A, llvm::StringRef B,
      const LLVMScalarEquivalenceLimits &Limits = {}) {
  Source Left(A), Right(B);
  if (!Left.Module || !Right.Module)
    return {};
  auto BeforeA = Left.text(), BeforeB = Right.text();
  auto Result =
      checkLLVMScalarEquivalence(Left.function(), Right.function(), Limits);
  EXPECT_EQ(BeforeA, Left.text());
  EXPECT_EQ(BeforeB, Right.text());
  return Result;
}

// This evaluator is only for straight-line oracle cases with concrete input
// words. Production equivalence uses symbolic execution and complete domains.
inline std::optional<llvm::APInt>
evaluate(const LLVMScalarFunctionModel &Model,
         llvm::ArrayRef<llvm::APInt> Arguments, bool Defined = true) {
  using namespace symbolic;
  SymContext C;
  SymState State(C);
  SymExec Exec(C, State);
  for (unsigned N = 0; N < Arguments.size(); ++N)
    State.write(
        SymSpace::Register, Model.Arguments[N].Storage.Offset,
        C.mkConst(Arguments[N].zext(Model.Arguments[N].Storage.Size * 8)));
  State.write(SymSpace::Register, LLVMInterpreterDefinednessOffset,
              C.mkZero(8));
  for (const auto &Block : Model.Graph.Function.Blocks) {
    if (Block.StartAddr != Model.Graph.Function.Entry)
      continue;
    for (const auto &Op : Block.Ops) {
      auto Step = Exec.step(Op);
      EXPECT_EQ(Exec.unmodelledCount(), 0U);
      EXPECT_EQ(C.numVars(), 0U);
      if (Step == StepResult::Return) {
        auto Guard = C.asConst(State.read(SymSpace::Register,
                                          LLVMInterpreterDefinednessOffset, 1));
        EXPECT_TRUE(Guard);
        if (Guard)
          EXPECT_EQ(Guard->isZero(), Defined);
        return C.asConst(Exec.branchTarget());
      }
      EXPECT_EQ(Step, StepResult::Continue);
    }
  }
  ADD_FAILURE() << "oracle expected a straight-line return";
  return std::nullopt;
}
} // namespace neverd::analysis::scalar_test
#endif

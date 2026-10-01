//===- LLVMInterpreterModelTest.h - Independent source-model oracles
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_LLVM_INTERPRETER_MODEL_TEST_H
#define NEVERD_UNITTESTS_LLVM_INTERPRETER_MODEL_TEST_H

#include "gtest/gtest.h"

#include "neverd/analysis/LLVMInterpreterMachineState.h"
#include "neverd/analysis/LowIRRefinement.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::analysis::llvm_model_test {
using Status = LowIRRefinementStatus;
inline NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
inline NdVar t(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::tmp(Offset, Bytes);
}
inline NdVar n(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
inline LowOp op(NdOp Code, NdVar Output = {},
                std::initializer_list<NdVar> Inputs = {}) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}

struct Oracle {
  LowFunc Function;
  Oracle(std::initializer_list<LowOp> Operations = {}, NdVar Status = n(0)) {
    Function.Entry = 0x100;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = 0x100;
    for (auto O : Operations)
      B.Ops.push_back(O);
    B.Ops.push_back(op(NdOp::RETURN, {}, {Status}));
    // Each instruction is deliberately independent of importer boundaries.
    for (auto &O : B.Ops) {
      LowInstructionBoundary IB;
      IB.Address = B.StartAddr + B.InstructionBoundaries.size();
      IB.Size = 1;
      IB.FirstOp = B.InstructionBoundaries.size();
      IB.OpCount = 1;
      O.Addr = IB.Address;
      O.Seq = 0;
      if (O.Opcode == NdOp::RETURN) {
        IB.Control = LowInstructionControl::Return;
        IB.ControlFlags = LowInstructionControlFlag::Return;
      }
      B.InstructionBoundaries.push_back(IB);
    }
    B.EndAddr = B.StartAddr + B.Ops.size();
    Function.Blocks.push_back(std::move(B));
  }
};

class LLVMModel : public testing::Test {
protected:
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module;
  void parse(llvm::StringRef Body, llvm::StringRef Preamble = {},
             llvm::StringRef Attributes = {},
             llvm::StringRef ReturnAttrs = {}) {
    std::string IR = "target datalayout = \"e-p:64:64\"\n";
    IR += Preamble;
    IR += "\ndefine ";
    IR += ReturnAttrs;
    IR += " i64 @model(ptr %state) ";
    IR += Attributes;
    IR += " {\n";
    IR += Body;
    IR += "\n}\n";
    llvm::SMDiagnostic Error;
    Module = llvm::parseAssemblyString(IR, Error, Context);
    std::string Message;
    llvm::raw_string_ostream OS(Message);
    Error.print("LLVM model test", OS);
    ASSERT_TRUE(Module) << Message;
  }
  llvm::Function &function() { return *Module->getFunction("model"); }
  llvm::Expected<InterpreterMachineStateModel>
  model(const LLVMInterpreterModelLimits &Limits = {}) {
    return modelLLVMInterpreterMachineStateX64(function(), Limits);
  }
  void reject(llvm::StringRef Diagnostic = {}) {
    auto M = model();
    ASSERT_FALSE(static_cast<bool>(M));
    auto Message = llvm::toString(M.takeError());
    EXPECT_FALSE(Message.empty());
    if (!Diagnostic.empty())
      EXPECT_NE(Message.find(Diagnostic.str()), std::string::npos) << Message;
  }
  void expect(const Oracle &Reference, Status Expected = Status::Proved,
              const LowIRIndependenceContract &C =
                  llvmInterpreterMachineStateContract()) {
    auto M = model();
    ASSERT_TRUE(static_cast<bool>(M)) << llvm::toString(M.takeError());
    auto R =
        checkLowIRRefinement(M->Function, M->Instructions, Reference.Function,
                             C, LowIRRefinementWitness::LiftedBits);
    EXPECT_EQ(R.Status, Expected) << R.Diagnostic;
    EXPECT_EQ(R.Certificate.has_value(), Expected == Status::Proved);
  }
};
} // namespace neverd::analysis::llvm_model_test
#endif

//===- LLVMInterpreterMachineState.cpp - x64 state adapter ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/analysis/arch/x86_64/LLVMInterpreterMachineState.h"

#include "../../llvm/LLVMInterpreterModelInternal.h"

#include "llvm/Support/Errc.h"

namespace neverd::analysis {
LowIRIndependenceContract llvmInterpreterMachineStateContract() {
  LowIRIndependenceContract C;
  for (unsigned Offset = 0; Offset != sizeof(InterpreterMachineStateX64V1);
       Offset += 8)
    C.ReturnRegisters.push_back({Offset, 8});
  C.EntryConstants.push_back(
      {NdVar::reg(LLVMInterpreterDefinednessOffset, 1), 0});
  C.ReturnRegisters.push_back({LLVMInterpreterDefinednessOffset, 1});
  C.PreservedRegisters.push_back({LLVMInterpreterDefinednessOffset, 1});
  return C;
}
llvm::Expected<InterpreterMachineStateModel>
modelLLVMInterpreterMachineStateX64(const llvm::Function &Function,
                                    const LLVMInterpreterModelLimits &Limits) {
  try {
    return llvm_model::Builder(Function, Limits,
                               sizeof(InterpreterMachineStateX64V1))
        .build();
  } catch (const llvm_model::Failure &E) {
    return llvm::createStringError(E.Budget ? llvm::errc::result_out_of_range
                                            : llvm::errc::invalid_argument,
                                   "%s", E.Message.c_str());
  }
}
} // namespace neverd::analysis

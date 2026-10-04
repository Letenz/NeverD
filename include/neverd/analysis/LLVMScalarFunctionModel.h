//===- LLVMScalarFunctionModel.h - Pure scalar LLVM model -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LLVMSCALARFUNCTIONMODEL_H
#define NEVERD_ANALYSIS_LLVMSCALARFUNCTIONMODEL_H

#include "neverd/analysis/LLVMInterpreterModel.h"

namespace neverd::analysis {

struct LLVMScalarArgument {
  NdVar Storage;
  unsigned Bits = 0;
};

/// A pure scalar interface to the same LLVM model used for state wrappers.
/// Arguments occupy distinct private register ranges; these offsets are model
/// identities, not addresses or an inferred native ABI. A one-bit argument
/// must be zero-extended into its byte of storage. RETURN carries the result.
struct LLVMScalarFunctionModel {
  InterpreterMachineStateModel Graph;
  std::vector<LLVMScalarArgument> Arguments;
  unsigned ResultBits = 0;
};

/// Model a verified function with noundef integer arguments and an integer
/// result. Supports i1/i8/i16/i32/i64 subject to the existing scalar model's
/// instruction and attribute contract. Memory, pointer operations, ordinary
/// calls, exceptions, undef/poison and unmodeled instructions are rejected.
/// The caller must initialize LLVMInterpreterDefinednessOffset to zero and
/// prove that it remains zero on every admitted execution, as well as prove
/// termination and the complete return value. Model construction alone is
/// not an equivalence or ABI certificate. The source function is unchanged.
llvm::Expected<LLVMScalarFunctionModel>
modelLLVMScalarFunction(const llvm::Function &Function,
                        const LLVMInterpreterModelLimits &Limits = {});

} // namespace neverd::analysis

#endif

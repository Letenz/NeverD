//===- LLVMMemoryAnalysis.h - Shared LLVM memory facts ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_ANALYSIS_LLVMMEMORYANALYSIS_H
#define NEVERD_ANALYSIS_LLVMMEMORYANALYSIS_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>

namespace llvm {
class Instruction;
class Value;
} // namespace llvm

namespace neverd::analysis {

/// Calls retain memory facts only under this complete intrinsic contract.
/// The call itself and its result must remain in place.
bool isLLVMMemoryTransparentIntrinsic(const llvm::Instruction &Instruction);

/// The stricter arithmetic-only subset also permits introducing private
/// storage. Memory-transparent stack/environment observers do not qualify.
bool isLLVMFrameIndependentIntrinsic(const llvm::Instruction &Instruction);

struct LLVMIntegerOffset {
  llvm::Value *Root;
  llvm::APInt Offset;
};

/// Split only full-width integer add/subtract chains with literal offsets.
/// Other operations remain exact SSA roots. No range, definedness, alias or
/// pointer-provenance fact is inferred. Charge is called for every visited
/// value; refusal returns no relation.
std::optional<LLVMIntegerOffset>
splitLLVMIntegerOffset(llvm::Value *Value, llvm::APInt Offset,
                       llvm::function_ref<bool(uint64_t)> Charge);

} // namespace neverd::analysis
#endif

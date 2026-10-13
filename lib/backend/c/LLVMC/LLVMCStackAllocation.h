//===- LLVMCStackAllocation.h - Executable C stack allocation -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own the private C projection of counted and non-entry LLVM allocations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVM_C_STACK_ALLOCATION_H
#define NEVERD_BACKEND_C_LLVM_C_STACK_ALLOCATION_H

#include <cstdint>
#include <optional>

namespace llvm {
class CallBase;
class Function;
class Module;
} // namespace llvm

namespace neverd::llvmc {

struct CStackAllocation {
  uint64_t Stride;
  uint64_t Alignment;
  unsigned CountBits;
};

/// Reject unsupported physical stack operations before normalization/DCE.
void validateCStackAllocations(const llvm::Module &Module,
                               const llvm::Function *Only);

/// Replace runtime allocations only on the emitter's private IR clone.
void lowerCStackAllocations(llvm::Module &Module, const llvm::Function *Only);

bool isCStackAllocationFunction(const llvm::Function &Function);
std::optional<CStackAllocation> cStackAllocation(const llvm::CallBase &Call);

} // namespace neverd::llvmc

#endif

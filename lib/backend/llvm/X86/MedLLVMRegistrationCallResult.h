//===- MedLLVMRegistrationCallResult.h - PE32 dead uses ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONCALLRESULT_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONCALLRESULT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <optional>

namespace llvm {
class AllocaInst;
class CallInst;
class Instruction;
} // namespace llvm
namespace neverd::med_llvm_eh {
/// A deletion order for only integer transports ending in store-only emitter
/// slots. Source memory, loads, metadata, escapes and other users reject.
std::optional<llvm::SmallVector<llvm::Instruction *, 8>>
unusedRegistrationCallResultPlan(
    llvm::CallInst &Call, llvm::ArrayRef<llvm::AllocaInst *> PrivateSlots,
    size_t &Work);
} // namespace neverd::med_llvm_eh

#endif

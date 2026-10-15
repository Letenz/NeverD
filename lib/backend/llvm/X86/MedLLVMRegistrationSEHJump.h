//===- MedLLVMRegistrationSEHJump.h - Native jump buffers -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHJUMP_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHJUMP_H

#include "neverd/backend/llvm/X86RegistrationSEHJump.h"

#include <vector>

namespace llvm {
class AllocaInst;
class CallInst;
class Function;
class Value;
} // namespace llvm

namespace neverd::x86_registration {
struct SEHJumpPlan {
  std::map<llvm::CallInst *, X86RegistrationSEHJumpCall> Calls;
  std::vector<llvm::AllocaInst *> Registers;
};

struct SEHJumpCallbackCall {
  llvm::CallInst *Call = nullptr;
  llvm::Value *Buffer = nullptr;
  X86RegistrationSEHJumpStack Stack;
};

/// Bind cloned calls and recover their capture buffers after callback commit.
std::map<llvm::CallInst *, SEHJumpCallbackCall>
emitSEHJumpCallbackBindings(const SEHJumpPlan &Plan, llvm::Function &Parent,
                            const std::map<va_t, llvm::AllocaInst *> &Buffers,
                            const std::map<va_t, llvm::Function *> &Callbacks);

std::optional<SEHJumpPlan>
prepareSEHJumps(const MedFunc &Source, llvm::Function &Parent,
                llvm::AllocaInst &Frame, uint64_t EntrySP,
                const std::map<const llvm::CallInst *, va_t> &Calls);

/// Commit a preflighted buffer projection. The returned map replaces the old
/// source calls before protected invokes are constructed.
std::map<llvm::CallInst *, llvm::CallInst *>
emitSEHJumps(const SEHJumpPlan &Plan, llvm::Function &Parent,
             llvm::AllocaInst &Frame, uint64_t EntrySP, va_t Owner,
             const std::map<va_t, llvm::Function *> &Callbacks);

} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHJUMP_H

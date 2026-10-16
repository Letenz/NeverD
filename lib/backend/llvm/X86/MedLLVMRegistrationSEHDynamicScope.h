//===- MedLLVMRegistrationSEHDynamicScope.h - Scope choice -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHDYNAMICSCOPE_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHDYNAMICSCOPE_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>

namespace llvm {
class AllocaInst;
class BasicBlock;
class Function;
} // namespace llvm
namespace neverd {
struct MedBlock;
namespace x86_registration {
bool canEmitDynamicSEHScope(const llvm::BasicBlock &Body);
/// Wrap one source body without duplicating its effects. Return its number of
/// calls protected by the selected live state.
unsigned
emitDynamicSEHScope(llvm::BasicBlock &Body, llvm::AllocaInst &Frame,
                    uint64_t EntrySP, va_t Owner, const MedBlock &Source,
                    llvm::ArrayRef<int32_t> Levels,
                    const std::map<int32_t, llvm::BasicBlock *> &Unwinds,
                    llvm::Function &TryBegin, llvm::Function &TryEnd);
} // namespace x86_registration
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHDYNAMICSCOPE_H

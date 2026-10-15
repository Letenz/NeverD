//===- MedLLVMRegistrationSEHState.h - Callback state writes ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind source TryLevel stores before outlining and update the physical parent
/// registration through LLVM's semantic state-write protocol after commit.

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHSTATE_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHSTATE_H

#include "neverd/Common.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"

#include <array>
#include <map>
#include <optional>
#include <vector>

namespace neverd {
struct MedFunc;
namespace x86_registration {

struct SEHCallbackStateWrite {
  const llvm::StoreInst *Original = nullptr;
  size_t Callback = 0;
  int32_t Level = 0;
  std::array<uint64_t, 4> Digest{};
  uint64_t ID = 0;
};

bool canPreserveSEHCallbackABI(const llvm::Function &Parent);
bool canEmitSEHRetainedScopes(const llvm::Function &Parent);

std::optional<std::vector<SEHCallbackStateWrite>> prepareSEHCallbackStateWrites(
    const MedFunc &Source, llvm::Function &Parent,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Memory);

/// The original source store remains observable in the recovered source frame.
/// The adjacent intrinsic writes the corresponding compiler-assigned state in
/// the actual registration, using the outliner's exact parent frame identity.
void emitSEHCallbackStateWrites(
    llvm::ArrayRef<SEHCallbackStateWrite> Writes,
    llvm::ArrayRef<llvm::Function *> Callbacks,
    llvm::ArrayRef<X86RegistrationCallbackInstructionMap> InstructionMaps,
    llvm::Function &Parent);

/// Keep callback-selected rows reachable without activating them on entry.
void emitSEHRetainedScopes(llvm::Function &Parent, va_t SourceVA,
                           llvm::ArrayRef<uint32_t> Scopes,
                           llvm::ArrayRef<llvm::BasicBlock *> Unwinds);

} // namespace x86_registration
} // namespace neverd

#endif

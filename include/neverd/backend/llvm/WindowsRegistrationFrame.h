//===- WindowsRegistrationFrame.h - x86 runtime callback frames -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_WINDOWSREGISTRATIONFRAME_H
#define NEVERD_BACKEND_LLVM_WINDOWSREGISTRATIONFRAME_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace llvm {
class AllocaInst;
class BasicBlock;
class Function;
} // namespace llvm

namespace neverd {

enum class X86RegistrationCallbackKind { Filter, Finally };

struct X86RegistrationCallbackFrame {
  /// When a lifted filter reads the original registration's exception-pointer
  /// cell, copy the runtime-provided pointer into this exact synthetic-frame
  /// cell. The producer must prove the source-frame offset; this helper only
  /// checks the alloca bounds and implements the runtime ABI.
  llvm::AllocaInst *ExceptionPointersFrame = nullptr;
  std::optional<uint64_t> ExceptionPointersOffset;

  /// Undefined ESP live-ins of the outlined callback use a private stack,
  /// rather than the parent's entry ESP or the runtime dispatcher's stack.
  /// The producer supplies a proven bound on callback stack use.
  llvm::ArrayRef<llvm::AllocaInst *> StackPointerSlots;
  uint32_t StackBytes = 0;
};

/// Outline a closed ordinary-flow callback from an emitted PE32 function.
/// Filters use i32(), frameaddress(1), eh.recoverfp and localrecover. Finally
/// callbacks use void(i8, ptr), recovering locals from their parent-FP
/// argument. Every referenced parent alloca/argument is escaped by identity,
/// with any existing localescape indices preserved. Only checked, entry-local
/// address computations can cross the function boundary; other values fail
/// explicitly.
///
/// All rejection checks precede mutation. The original blocks remain in the
/// parent until its native EH dispatcher has been committed. This operation
/// alone is not a native exception reconstruction receipt.
llvm::Expected<llvm::Function *> outlineX86RegistrationCallback(
    llvm::Function &Parent, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Blocks, X86RegistrationCallbackKind Kind,
    llvm::StringRef Name, const X86RegistrationCallbackFrame &Frame = {});

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSREGISTRATIONFRAME_H

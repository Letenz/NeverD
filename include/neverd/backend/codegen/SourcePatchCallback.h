//===- SourcePatchCallback.h - Original entry relocation ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Machine-code evidence for a callable copy of an original entry prefix.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_SOURCEPATCHCALLBACK_H
#define NEVERD_BACKEND_CODEGEN_SOURCEPATCHCALLBACK_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <vector>

namespace neverd {

struct ExceptionFunction;
struct DwarfCIE;
struct DwarfFDE;

struct SourceCallbackInstruction {
  uint32_t Offset = 0;
  uint16_t Size = 0;
};

struct SourceCallbackPrefix {
  va_t OriginalVA = 0;
  va_t CallbackVA = 0;
  uint32_t OverwriteSize = 0;
  uint32_t CopiedSize = 0;
  InstructionMode Mode = InstructionMode::Default;
  /// Relocated complete instructions followed by a register-neutral branch
  /// to OriginalVA + CopiedSize. No scratch-register or stack changes.
  std::vector<uint8_t> Bytes;
  std::vector<SourceCallbackInstruction> Instructions;
};

/// Relocate a straight-line prefix with an explicit instruction contract.
/// This is the machine-code layer only: callers must authenticate the function
/// extent, incoming edges, loader relocations and unwind records separately.
/// The prefix cannot contain calls, returns, traps, conditional blocks or
/// unmodeled PC-observing instructions. A rejected shape is never copied as a
/// guessed position-independent instruction. The original buffer is read-only.
llvm::Expected<SourceCallbackPrefix>
relocateSourceCallbackPrefix(Arch Architecture, InstructionMode Mode,
                             llvm::ArrayRef<uint8_t> Original, va_t OriginalVA,
                             va_t CallbackVA);

/// Describe the exact relocated instruction states, including the final
/// continuation branch. Does not reuse personality/LSDA or PC-dependent CFI.
llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackDwarfFrame(const SourceCallbackPrefix &Prefix,
                               const DwarfCIE &CIE, const DwarfFDE &FDE,
                               uint64_t FrameVA);

/// Derive frame states from an explicitly admitted machine prologue when the
/// original only carries a completed-prologue compact recipe.
llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackMachineDwarf(const SourceCallbackPrefix &Prefix,
                                 Arch Architecture, uint64_t FrameVA);
llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackEHABI(const SourceCallbackPrefix &Prefix);

/// Persist exact AArch32 callback mode and extent through native code symbols.
/// Existing symbol indices and loader identities retain their meanings.
llvm::Error
registerSourceCallbackSymbols(std::vector<uint8_t> &Binary, BinaryFormat Format,
                              llvm::ArrayRef<SourceCallbackPrefix> Prefixes);

/// Windows table unwind for the exact copied prologue, with no language
/// handler or epilogue attached to the jump-only continuation.
llvm::Expected<std::vector<uint8_t>>
encodeSourceCallbackCOFFUnwind(const SourceCallbackPrefix &Prefix,
                               const ExceptionFunction &Function,
                               Arch Architecture);

} // namespace neverd

#endif

//===- DirectBranch.h - Direct branch decoding and scanning ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the direct calls and jumps of x86, AArch64 and 32-bit ARM, the one
/// place that reads their encodings.  A caller names the kinds of branch it
/// accepts and gets back the instruction's kind and length and the base and
/// displacement of its target, so each caller keeps its own view of the
/// address space.
///
/// A loader uses it to find the sites that reach a known runtime entry point
/// before any disassembler has run.  A byte pattern alone would be far too
/// weak on a variable-length encoding.  What makes that scan sound is that a
/// decoded target is kept only when it lands exactly on an address the caller
/// already proved is a runtime entry, which a misaligned match will not do.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DIRECTBRANCH_H
#define NEVERD_LOADER_DIRECTBRANCH_H

#include "neverd/Common.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd {

/// The kinds of direct branch decodeDirectBranch tells apart.
enum class DirectBranchKind : uint8_t {
  /// An unconditional jump: x86 `jmp rel32`, A64 `b`, A32 `b` and T32 `b.w`.
  Jump,
  /// A call: x86 `call rel32`, A64 `bl`, A32 `bl` and T32 `bl`.
  Call,
  /// A call that enters the other ARM instruction set: A32 and T32 `blx`
  /// (immediate).
  ExchangingCall,
};

/// The direct branches a caller accepts.
struct DirectBranchForms {
  bool Jumps = false;
  bool Calls = false;
  bool ExchangingCalls = false;
  /// Also the A32 jumps and calls under a condition other than always.
  bool Conditional = false;
};

/// A decoded direct branch.
struct DirectBranch {
  DirectBranchKind Kind = DirectBranchKind::Jump;
  /// An A32 branch under a condition other than always.
  bool Conditional = false;
  /// The instruction's address and size.
  va_t Address = 0;
  unsigned Length = 0;
  /// What the displacement counts from: the address plus the distance the
  /// program counter reads ahead, word-aligned for a T32 `blx`, modulo 2^64.
  va_t Base = 0;
  int64_t Displacement = 0;
  /// Whether the target runs in Thumb state; only a 32-bit ARM branch can say
  /// so.  The target itself never carries the interworking bit.
  bool TargetIsThumb = false;

  /// The target, as 64-bit address arithmetic gives it: past the top of the
  /// address space it wraps around.
  va_t wrappingTarget() const { return Base + static_cast<va_t>(Displacement); }

  /// The target, or std::nullopt when reaching it wraps around the address
  /// space.
  std::optional<va_t> target() const;
};

/// Decode a direct branch of one of \p Forms at \p VA, reading at most
/// \p Available bytes from \p Code.  \p Mode selects between the two ARM
/// instruction sets, whose branches share no encoding; it is ignored on every
/// other architecture, and a mode that is not one instruction set decodes
/// nothing.
///
/// Not decoded are x86's short and conditional jumps, A64's conditional and
/// compare-and-branch forms, T32's conditional `b.w` and its 16-bit branches,
/// and a T32 `blx` with its H bit set, which is UNDEFINED.
std::optional<DirectBranch> decodeDirectBranch(Arch A, InstructionMode Mode,
                                               const uint8_t *Code,
                                               size_t Available, va_t VA,
                                               DirectBranchForms Forms);

/// How many bytes a direct branch of \p A takes.
size_t getDirectBranchLength(Arch A);

/// Decode a direct call or unconditional direct jump at \p VA, reading at most
/// \p Available bytes from \p Code.  Both forms matter: a runtime helper that
/// returns is called, while one that does not return is reached by a tail jump
/// as often as by a call.  From Thumb code a `blx` call is one too; an A32
/// `blx` and a conditional branch are not.
///
/// The returned address never carries the ARM Thumb interworking bit, so a
/// caller matching it against symbol-derived addresses must normalize those
/// the same way (see \ref normalizeCodeAddress).  A target that wraps around
/// the address space is not returned.
///
/// On success \p Length receives the size of the decoded instruction.
std::optional<va_t> decodeDirectBranchTarget(Arch A, InstructionMode Mode,
                                             const uint8_t *Code,
                                             size_t Available, va_t VA,
                                             size_t &Length);

/// Step between branch candidates.  Fixed-width targets are scanned at
/// instruction granularity; x86 must be scanned byte by byte because a call
/// can begin at any offset, and Thumb at halfword granularity because its
/// 16- and 32-bit instructions interleave freely.
unsigned getBranchScanStride(Arch A, InstructionMode Mode);

/// True for the targets \ref decodeDirectBranchTarget models.
inline bool canScanDirectBranches(Arch A, InstructionMode Mode) {
  return isSingleInstructionMode(Mode) &&
         (A == Arch::X64 || A == Arch::X86 || A == Arch::AArch64 ||
          A == Arch::ARM);
}

/// Call \p Visit(SiteVA, TargetVA) for every direct branch in
/// [\p BeginVA, \p BeginVA + \p Size) whose bytes start at \p Code.
template <typename Fn>
void forEachDirectBranch(Arch A, InstructionMode Mode, const uint8_t *Code,
                         size_t Size, va_t BeginVA, Fn Visit) {
  if (!canScanDirectBranches(A, Mode))
    return;
  const unsigned Stride = getBranchScanStride(A, Mode);
  const size_t Length = getDirectBranchLength(A);
  for (size_t Offset = 0; Offset + Length <= Size; Offset += Stride) {
    size_t Decoded = 0;
    std::optional<va_t> Target = decodeDirectBranchTarget(
        A, Mode, Code + Offset, Size - Offset, BeginVA + Offset, Decoded);
    if (Target)
      Visit(BeginVA + Offset, *Target);
  }
}

} // namespace neverd

#endif // NEVERD_LOADER_DIRECTBRANCH_H

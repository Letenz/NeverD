//===- InstructionFlow.h - What each native instruction does ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What the C API publishes about each decoded native instruction: its
/// control transfer, the memory it reaches through constant addresses and
/// how it moves the stack pointer, all from the instruction's own LowIR lift
/// so that a listing agrees with the operations the pipeline consumes.  Also
/// the paged, parallel decode of whole functions that reference queries
/// share.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_INSTRUCTIONFLOW_H
#define NEVERD_LIB_SDK_CAPI_INSTRUCTIONFLOW_H

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <vector>

namespace neverd::sdk {

struct FuncInfo;
struct Session;

/// Linear native decode from \p Addr, bounded by \p Span bytes when nonzero
/// and by \p Limit instructions.  Mode selection, segment bounds and ARM state
/// checks are those of the published disassembly; \p Visit returns false to
/// stop after an instruction.
template <typename VisitorT>
bool decodeNativeRange(const BinaryImage &Img, Decoder &Dec, va_t Addr,
                       uint64_t Span, uint64_t Limit, VisitorT Visit) {
  va_t Cur = Addr;
  std::optional<InstructionMode> PathMode = Img.instructionModeAt(Addr);
  for (uint64_t I = 0; I < Limit; ++I) {
    if (Cur < Addr)
      break;
    const Segment *Seg = Img.getSegmentFor(Cur);
    if (!Seg || !Seg->isExecutable())
      break;

    uint64_t Consumed = Cur - Addr;
    if (Span > 0 && Consumed >= Span)
      break;

    uint64_t Off64 = Cur - Seg->VA;
    if (Off64 >= Seg->Data.size())
      break;
    size_t Off = static_cast<size_t>(Off64);
    uint64_t Avail64 = std::min<uint64_t>(
        16, std::min<uint64_t>(Seg->Size - Off64, Seg->Data.size() - Off));
    if (Span > 0)
      Avail64 = std::min(Avail64, Span - Consumed);
    if (Avail64 == 0)
      break;
    const uint8_t *Bytes = Seg->Data.data() + Off;

    if (!Dec.selectMode(Img, Cur, PathMode))
      return false; // Unknown or conflicting instruction mode.
    PathMode = Dec.currentMode();
    DecodedInsn DI;
    int Sz = Dec.decodeOne(Bytes, static_cast<size_t>(Avail64), Cur, DI);
    if (Sz <= 0)
      break;
    if (Img.Arch == Arch::ARM &&
        Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
            Dec.currentMode())
      break;
    if (!Visit(DI, Bytes, Sz))
      break;
    if (static_cast<uint64_t>(Sz) > InvalidVA - Cur)
      break;
    Cur += Sz;
  }
  return true;
}

/// Where an instruction leaves the stack pointer: its value before the
/// instruction plus Delta, or Base's value before it plus Delta.  Unknown
/// when the lift does not reduce the new value to either.
struct StackMove {
  int64_t Delta = 0;
  std::optional<uint64_t> Base;
  bool Unknown = false;
};

/// Control transfer and constant memory references of one decoded native
/// instruction.  Kind is empty for a fall-through instruction.
struct InstructionFlow {
  llvm::StringRef Kind;
  va_t Target = InvalidVA;
  llvm::SmallVector<std::pair<va_t, llvm::StringRef>, 2> Refs;
  StackMove Stack;
};

/// The flow kind of an instruction the lifter cannot model.
inline constexpr llvm::StringLiteral UnliftedFlow = "unlifted";

/// The flow of \p DI, with its stack pointer move when \p StackRegisters is
/// given.
InstructionFlow
summarizeInstructionFlow(const BinaryImage &Img, Decoder &Dec,
                         const DecodedInsn &DI,
                         const TargetRegInfo *StackRegisters = nullptr);

/// The pointer the loader stored in a relocated slot, as a code address when
/// the slot points to code.
std::optional<va_t> relocatedPointer(const BinaryImage &Img, va_t Slot);

/// The functions one page of a reference query covers: from \p FirstEntry
/// in entry order, at most \p MaxFunctions (1 to MaxFunctionsPerPage), and
/// the entry the next page starts at.
struct FunctionPage {
  static constexpr int MaxFunctionsPerPage = 4096;
  std::vector<const FuncInfo *> Functions;
  std::optional<va_t> NextEntry;
};
FunctionPage functionPage(const Session &S, va_t FirstEntry, int MaxFunctions);

/// Decodes each function of \p Page with a decoder of its own, functions in
/// parallel, and calls \p Visit(Index, Decoder, Instruction) for each of
/// its instructions in order: up to the function's recorded size, or to its
/// first terminator when it has none.  One function's visits run on one
/// thread, so writing only to its own output keeps the result independent
/// of the thread count.  False when a decoder could not be set up.
bool decodeFunctions(
    const Session &S, const FunctionPage &Page,
    llvm::function_ref<void(size_t Index, Decoder &, const DecodedInsn &)>
        Visit);

} // namespace neverd::sdk

#endif // NEVERD_LIB_SDK_CAPI_INSTRUCTIONFLOW_H

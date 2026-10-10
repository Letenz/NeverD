//===- RegistrationStateRealignment.cpp - x86 EH frame coordinates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <utility>

namespace neverd::registration_state {

bool RegistrationStateSolver::validateRealignedLayout() {
  const auto &Layout = *Chain.RealignedFrame;
  if (!KnownCxx || Layout.BaseRegister != 6 ||
      Function.Entry > UINT32_MAX - 17 ||
      Layout.DefinitionVA != Function.Entry + 15 || Layout.Alignment < 4 ||
      Layout.Alignment > 128 || (Layout.Alignment & (Layout.Alignment - 1)) ||
      Layout.AllocationBytes < 20 ||
      Layout.AllocationBytes > limits::kMaxRegistrationEHStateWork ||
      Layout.BaseOffset > -20 ||
      int64_t(Layout.BaseOffset) + Layout.AllocationBytes < 0 ||
      Layout.SavedParentFrameOffset != -20 || Chain.RegistrationOffset != -12 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != -1)
    return false;
  // The coordinate change must occur on the ordinary entry path. Metadata
  // alone cannot seed ESI or skip the LowIR prologue that constructs it.
  const auto Entry = llvm::find_if(Function.Blocks, [&](const LowBlock &Block) {
    return Block.StartAddr == Function.Entry;
  });
  if (Entry == Function.Blocks.end() || Entry->EndAddr < Function.Entry + 17)
    return false;
  static constexpr std::pair<unsigned, unsigned> Prologue[] = {
      {0, 1}, {1, 2}, {3, 1}, {4, 1}, {5, 1}, {6, 3}, {9, 6}, {15, 2}};
  for (const auto &[Offset, Size] : Prologue) {
    unsigned Matches = 0;
    for (const auto &Boundary : Entry->InstructionBoundaries) {
      if (!charge(1))
        return false;
      if (Boundary.Address == Function.Entry + Offset &&
          Boundary.Size == Size &&
          Boundary.Control == LowInstructionControl::None)
        ++Matches;
    }
    if (Matches != 1)
      return false;
  }
  return true;
}

bool RegistrationStateSolver::realignedMemoryIsDisjoint(
    const FrameValue &Address, uint16_t Width) const {
  if (!Chain.RealignedFrame)
    return true;
  if (Address.EntryOffset)
    return *Address.EntryOffset >= -12;
  if (Address.Offset) {
    const auto &Layout = *Chain.RealignedFrame;
    const int64_t AlignedTop =
        int64_t(Layout.BaseOffset) + Layout.AllocationBytes;
    return int64_t(*Address.Offset) + Width <= AlignedTop;
  }
  return true;
}

bool RegistrationStateSolver::realignedInstallationReady(
    const FrameState &Frame) const {
  if (!Chain.RealignedFrame)
    return true;
  const auto &Layout = *Chain.RealignedFrame;
  const auto Parent = Frame.load(Layout.SavedParentFrameOffset, 4);
  const auto Stack = Frame.load(*Chain.RegistrationOffset - 4, 4);
  return Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride].EntryOffset ==
             0 &&
         Frame.Registers[x86reg::RSI / x86reg::GeneralRegStride].Offset ==
             Layout.BaseOffset &&
         Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset ==
             Layout.BaseOffset &&
         Parent.EntryOffset == 0 && Stack.Offset == Layout.BaseOffset &&
         Frame.load(*Chain.RegistrationOffset, 4).PreviousChain &&
         Frame.load(*Chain.RegistrationOffset + 4, 4).Constant ==
             Chain.HandlerVA &&
         Frame.load(*Chain.TryLevelOffset, 4).Constant ==
             uint32_t(*Chain.SeededTryLevel);
}

} // namespace neverd::registration_state

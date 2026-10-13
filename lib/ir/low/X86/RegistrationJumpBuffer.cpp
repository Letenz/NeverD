//===- RegistrationJumpBuffer.cpp - Live EH3 capture storage --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Tracks the initialized fields and lifetime of an EH3 jump buffer.

#include "RegistrationJumpCapture.h"

namespace neverd::registration_state {
namespace {
std::array<FrameValue, 10> captureCells(const EH3JumpCapture &Capture) {
  return {Capture.Nonvolatile[0],
          Capture.Nonvolatile[1],
          Capture.Nonvolatile[2],
          Capture.Nonvolatile[3],
          FrameValue::frame(Capture.StackOffset - 4),
          FrameValue::constant(uint32_t(Capture.ReturnPC)),
          FrameValue::frame(-16),
          FrameValue::constant(uint32_t(Capture.Level)),
          FrameValue::constant(0x56433230),
          FrameValue::constant(uint32_t(Capture.UnwindTarget))};
}
} // namespace

void storeEH3JumpCapture(FrameState &Frame, const EH3JumpCapture &Capture) {
  const auto Cells = captureCells(Capture);
  for (unsigned I = 0; I != Cells.size(); ++I)
    Frame.store(Capture.BufferOffset + 4 * I, 4, Cells[I]);
}

bool hasEH3JumpCapture(const FrameState &Frame, const EH3JumpCapture &Capture) {
  const auto Cells = captureCells(Capture);
  for (unsigned I = 0; I != Cells.size(); ++I)
    if (Frame.load(Capture.BufferOffset + 4 * I, 4) != Cells[I])
      return false;
  return true;
}

} // namespace neverd::registration_state

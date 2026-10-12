//===- RegistrationJumpCapture.h - Bound EH3 jump capture ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind the current EH3 invocation to its source jump-buffer capture.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONJUMPCAPTURE_H
#define NEVERD_IR_LOW_X86_REGISTRATIONJUMPCAPTURE_H

#include "RegistrationFrame.h"

#include <array>
#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;

namespace registration_state {

struct EH3JumpCapture {
  static constexpr uint16_t BufferBytes = 64;
  va_t Call = InvalidVA;
  va_t ReturnPC = InvalidVA;
  va_t UnwindTarget = InvalidVA;
  int32_t BufferOffset = 0;
  int32_t StackOffset = 0;
  int32_t Level = -1;
  /// Actual EBP, EBX, EDI and ESI values. A later resume must retain their
  /// provenance; an unknown saved register is never a fabricated scalar.
  std::array<FrameValue, 4> Nonvolatile;
};

/// Prepare a capture for a checked installed direct EH3 frame. Only the
/// two-argument EH3 unwind protocol is admitted. Buffer storage must be fully
/// separated from outgoing arguments and registration administration. This
/// supplies no surviving-buffer or resumption proof and is not a call-pop
/// summary. Unknown roles, source slices and protocol values refuse.
std::optional<EH3JumpCapture>
prepareEH3JumpCapture(const BinaryImage &Image, const LowFunc &Function,
                      const LowOp &Op, const FrameState &Frame, bool Installed,
                      size_t &Work);

} // namespace registration_state
} // namespace neverd

#endif

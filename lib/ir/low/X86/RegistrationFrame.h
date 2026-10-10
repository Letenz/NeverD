//===- RegistrationFrame.h - x86 EH frame values --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONFRAME_H

#include "neverd/Common.h"

#include <array>
#include <cstddef>
#include <map>
#include <optional>

namespace neverd {
struct LowOp;
struct NdVar;
} // namespace neverd

namespace neverd::registration_state {

/// Values relative to the established EBP, in the PE32 address domain. Unknown
/// values retain possible frame provenance when paths or arithmetic disagree.
struct FrameValue {
  std::optional<int32_t> Offset;
  std::optional<uint32_t> Constant;
  bool PreviousChain = false;
  bool MayBeFrame = false;
  /// An opaque call result is not a proven local/caller frame address. Keep
  /// its possible provenance for memory and escape sinks, while allowing an
  /// otherwise private preserved helper to return it to its checked caller.
  bool FrameOnlyFromCall = false;
  uint8_t SavedRegister = 0;
  bool ReturnPC = false;
  /// Exact PE32 cookie expression: Cookie ^ Frame(Offset) ^ Constant, or
  /// Cookie ^ Constant when the frame offset is absent. Other arithmetic and
  /// partial-width aliases drop this identity while retaining frame taint.
  bool SecurityCookie = false;
  std::optional<int32_t> CookieFrameOffset;
  uint32_t CookieXOR = 0;

  static FrameValue frame(int32_t Offset) { return {Offset, {}, false, true}; }
  static FrameValue constant(uint32_t Value) {
    return {{}, Value, false, false};
  }
  static FrameValue previousChain() { return {{}, {}, true, false}; }
  friend bool operator==(const FrameValue &, const FrameValue &) = default;
};

FrameValue join(FrameValue Left, const FrameValue &Right);

struct FrameState {
  std::array<FrameValue, 8> Registers;
  /// Flags and vector registers carry conservative byte taint. They cannot
  /// acquire an exact frame offset through a truncation or vector alias.
  std::map<uint64_t, FrameValue> OtherRegisterBytes;
  /// Calls may define any volatile flag/vector byte. Explicit subsequent
  /// writes override this default; a missing map entry is not a zero value.
  bool OtherRegistersMayBeFrame = false;
  std::map<int32_t, FrameValue> Cells;

  bool merge(const FrameState &Other);

  void store(int32_t Offset, uint16_t Width, const FrameValue &Value);

  FrameValue load(std::optional<int32_t> Offset, uint16_t Width) const;
};

/// One block transfer. Registers and frame cells survive instructions and CFG
/// edges; only the lifter's instruction-local temporaries are discarded.
class FrameTransfer {
public:
  FrameTransfer(FrameState &State, int32_t RegistrationOffset,
                va_t SecurityCookieVA = 0)
      : State(State), RegistrationOffset(RegistrationOffset),
        SecurityCookieVA(SecurityCookieVA) {}

  void beginInstruction(va_t Address);

  FrameValue read(const NdVar &Value) const;

  FrameValue evaluate(const LowOp &Op, bool Installed) const;

  bool isCookieCheck(const LowOp &Op, va_t CookieCheckVA) const;

  void write(const LowOp &Op, FrameValue Value, va_t CookieCheckVA = 0);

private:
  FrameState &State;
  int32_t RegistrationOffset;
  va_t SecurityCookieVA;
  std::map<uint64_t, FrameValue> Temps;
  va_t Instruction = InvalidVA;
};

} // namespace neverd::registration_state

#endif

//===- RegistrationControl.h - PE32 code target widths -----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares checked source control contracts for PE32 registration EH.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCONTROL_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCONTROL_H

#include "neverd/ir/low/LowIR.h"

namespace neverd::registration_state {

/// LowIR code addresses use the common VA width; PE32 data pointers remain
/// four bytes. Either explicit representation must name an exact PE32 VA.
inline bool isPE32CodeTarget(const NdVar &Value) {
  return Value.isConst() && (Value.Size == 4 || Value.Size == 8) &&
         Value.Offset <= UINT32_MAX;
}

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONCONTROL_H

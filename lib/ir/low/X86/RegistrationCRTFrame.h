//===- RegistrationCRTFrame.h - Checked PE32 CRT frame effects --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares checked PE32 CRT frame effects.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONCRTFRAME_H
#define NEVERD_IR_LOW_X86_REGISTRATIONCRTFRAME_H

#include "neverd/Common.h"

namespace neverd {
struct BinaryImage;
struct LowOp;
namespace registration_state {
bool isCheckedControlFPSlot(const BinaryImage &Image, va_t Slot);
bool isCheckedControlFPCall(const BinaryImage &Image, const LowOp &Op,
                            size_t &Work);
} // namespace registration_state
} // namespace neverd

#endif

//===- SEHImageFormat.h - Live Windows image formats ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks source-written format bytes without trusting writable file data.

#ifndef NEVERD_IR_LOW_WINDOWS_SEHIMAGEFORMAT_H
#define NEVERD_IR_LOW_WINDOWS_SEHIMAGEFORMAT_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstddef>

namespace neverd::seh_frame {
struct State;

bool checkedImageFormat(const State &Caller, va_t Address, size_t &Remaining);

} // namespace neverd::seh_frame

#endif

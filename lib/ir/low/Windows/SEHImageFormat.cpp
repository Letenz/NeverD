//===- SEHImageFormat.cpp - Live Windows image formats -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Applies the Windows scalar grammar to bytes written in this invocation.

#include "SEHImageFormat.h"

#include "../SEHFrameState.h"

#include "neverd/ir/low/SEHRuntimeMemory.h"

#include <string>

namespace neverd::seh_frame {

bool checkedImageFormat(const State &Caller, va_t Address, size_t &Remaining) {
  std::string Format;
  for (size_t I = 0; I != kMaxWindowsSEHFormatBytes; ++I) {
    if (!Remaining || Address > InvalidVA - I)
      return false;
    --Remaining;
    const auto Byte = Caller.ImageBytes.find(Address + I);
    if (Byte == Caller.ImageBytes.end())
      return false;
    if (Byte->second) {
      Format.push_back(char(Byte->second));
      continue;
    }
    if (Format.size() + 1 > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= Format.size() + 1;
    return isWindowsSEHScalarFormat(Format);
  }
  return false;
}

} // namespace neverd::seh_frame

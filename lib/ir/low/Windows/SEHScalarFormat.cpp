//===- SEHScalarFormat.cpp - Windows scalar format grammar ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns the bounded Windows scalar grammar used by image and frame formats.

#include "neverd/ir/low/SEHRuntimeMemory.h"

#include <cstdint>

namespace neverd {
namespace {

bool consumeDecimal(llvm::StringRef &Format) {
  uint32_t Value = 0;
  while (!Format.empty() && Format.front() >= '0' && Format.front() <= '9') {
    const unsigned Digit = Format.front() - '0';
    if (Value > (uint32_t(INT32_MAX) - Digit) / 10)
      return false;
    Value = Value * 10 + Digit;
    Format = Format.drop_front();
  }
  return true;
}

} // namespace

bool isWindowsSEHScalarFormat(llvm::StringRef Format) {
  if (Format.size() >= kMaxWindowsSEHFormatBytes)
    return false;
  unsigned Arguments = 0;
  while (!Format.empty()) {
    const char Character = Format.front();
    Format = Format.drop_front();
    if (!Character || uint8_t(Character) >= 128)
      return false;
    if (Character != '%')
      continue;
    if (Format.consume_front("%"))
      continue;
    while (!Format.empty() && llvm::StringRef("-+ #0").contains(Format.front()))
      Format = Format.drop_front();
    if (!consumeDecimal(Format))
      return false;
    const bool Precision = Format.consume_front(".");
    if (Precision && !consumeDecimal(Format))
      return false;
    llvm::StringRef Length;
    if (!Format.empty() && llvm::StringRef("hljzt").contains(Format.front())) {
      unsigned Size = 1;
      if (Format.starts_with("hh") || Format.starts_with("ll"))
        Size = 2;
      Length = Format.take_front(Size);
      Format = Format.drop_front(Size);
    }
    if (Format.empty())
      return false;
    const char Conversion = Format.front();
    Format = Format.drop_front();
    if (llvm::StringRef("diouxX").contains(Conversion)) {
      // Every admitted length denotes a scalar integer in the Windows CRT.
    } else if (llvm::StringRef("aAeEfFgG").contains(Conversion)) {
      if (!Length.empty() && Length != "l")
        return false;
    } else if (Conversion == 'c') {
      if (!Length.empty() || Precision)
        return false;
    } else {
      // Positional and foreign dialects, pointer conversions and dynamic
      // widths need their own runtime argument and callback contracts.
      return false;
    }
    if (++Arguments > 64)
      return false;
  }
  return true;
}

} // namespace neverd

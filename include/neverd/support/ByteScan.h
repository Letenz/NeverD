//===- ByteScan.h - Bounded text byte scans ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared ASCII admission and optional bulk UTF-8 validation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_BYTESCAN_H
#define NEVERD_SUPPORT_BYTESCAN_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace neverd {

/// True only when the optional bulk validator proves the complete span valid.
/// False requires the caller's ordinary validation/repair path, including when
/// acceleration is disabled. Never reads padding outside Text.
bool knownValidUTF8(std::string_view Text);

namespace strings::detail {
inline bool isTextASCII(uint32_t C) {
  return (C >= 0x20 && C < 0x7f) || C == '\t' || C == '\n' || C == '\r';
}

size_t longTextASCIIPrefix(const uint8_t *Data, size_t Size);

/// Length of the initial ASCII text run, reading at most Size bytes.
inline size_t textASCIIPrefix(const uint8_t *Data, size_t Size) {
  // Most printable runs in binary images are tiny. Keep them inline and only
  // dispatch after proving a useful prefix, independent of the backend.
  const size_t Probe = Size < 16 ? Size : 16;
  size_t I = 0;
  for (; I < Probe; ++I)
    if (!isTextASCII(Data[I]))
      return I;
  return I == Size ? I : I + longTextASCIIPrefix(Data + I, Size - I);
}
} // namespace strings::detail
} // namespace neverd

#endif

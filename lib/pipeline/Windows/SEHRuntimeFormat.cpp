//===- SEHRuntimeFormat.cpp - Windows CRT format contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks a scalar subset of the Windows narrow printf grammar and storage.

#include "SEHRuntimeFormat.h"

#include "neverd/ir/low/SEHRuntimeMemory.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd {

std::optional<std::string> readWindowsSEHScalarFormat(const BinaryImage &Image,
                                                      va_t Address,
                                                      size_t &Remaining) {
  for (size_t I = 0; I != kMaxWindowsSEHFormatBytes; ++I) {
    if (!Remaining || Address > InvalidVA - I)
      return std::nullopt;
    --Remaining;
    const auto *Byte = Image.readVA(Address + I, 1);
    // Multibyte decoding depends on the current locale, which is unproved.
    if (!Byte || *Byte >= 128)
      return std::nullopt;
    if (*Byte)
      continue;
    const auto Bytes = readImmutableImageBytes(Image, Address, I + 1);
    if (!Bytes)
      return std::nullopt;
    std::string Format(Bytes->begin(), Bytes->end());
    if (!isWindowsSEHScalarFormat(llvm::StringRef(Format).drop_back()))
      return std::nullopt;
    return Format;
  }
  return std::nullopt;
}

} // namespace neverd

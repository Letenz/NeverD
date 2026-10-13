//===- SEHRuntimeFormat.cpp - Windows CRT format contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks a scalar subset of the Windows narrow printf grammar and storage.

#include "SEHRuntimeFormat.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"

#include "llvm/ADT/StringRef.h"

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

bool isScalarFormat(llvm::StringRef Format) {
  unsigned Arguments = 0;
  while (!Format.empty()) {
    const char Character = Format.front();
    Format = Format.drop_front();
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

} // namespace

std::optional<std::string> readWindowsSEHScalarFormat(const BinaryImage &Image,
                                                      va_t Address,
                                                      size_t &Remaining) {
  constexpr uint32_t MaxFormatBytes = 4096;
  for (uint32_t I = 0; I != MaxFormatBytes; ++I) {
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
    if (!isScalarFormat(llvm::StringRef(Format).drop_back()))
      return std::nullopt;
    return Format;
  }
  return std::nullopt;
}

} // namespace neverd

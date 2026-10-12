//===- Encoding.cpp - Native bounded Base64 decoding ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode captured metadata bytes without whitespace or residual-bit coercion.
///
//===----------------------------------------------------------------------===//

#include "Encoding.h"

namespace neverd::web::package_attestations {
DecodedBase64 decodeBase64(std::string_view Text, uint64_t MaxBytes,
                           bool AllowURL) {
  DecodedBase64 Result;
  if (Text.empty()) {
    Result.Status = "empty_base64";
    return Result;
  }
  const auto FirstPad = Text.find('=');
  const auto Data = Text.substr(0, FirstPad);
  const auto Pads = Text.size() - Data.size();
  if (Data.empty() || Pads > 2 || Data.size() % 4 == 1 ||
      (Pads && (Text.size() % 4 ||
                Text.substr(FirstPad).find_first_not_of('=') != Text.npos)))
    return Result;
  const uint64_t Size =
      Data.size() / 4 * 3 + (Data.size() % 4 ? Data.size() % 4 - 1 : 0);
  if (Size > MaxBytes) {
    Result.Status = "decoded_byte_budget_exceeded";
    return Result;
  }
  constexpr std::string_view Alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  bool Standard = false, URL = false;
  unsigned Bits = 0, Count = 0;
  std::string Bytes;
  Bytes.reserve(Size);
  for (const char C : Data) {
    Standard |= C == '+' || C == '/';
    URL |= C == '-' || C == '_';
    if ((URL && !AllowURL) || (Standard && URL)) {
      Result.Status = "unsupported_base64_alphabet";
      return Result;
    }
    const auto Digit = C == '-' ? 62 : C == '_' ? 63 : Alphabet.find(C);
    if (Digit == Alphabet.npos)
      return Result;
    Bits = (Bits << 6) | unsigned(Digit);
    Count += 6;
    if (Count >= 8) {
      Count -= 8;
      Bytes.push_back(char(Bits >> Count));
      Bits &= (1U << Count) - 1;
    }
  }
  if (Bits || Bytes.size() != Size)
    return Result;
  Result.Bytes = std::move(Bytes);
  Result.Status = URL ? "admitted_url_base64" : "admitted_standard_base64";
  return Result;
}
} // namespace neverd::web::package_attestations

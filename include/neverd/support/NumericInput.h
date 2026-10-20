//===- NumericInput.h - Numeric input ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Numbers entered in numeric SDK and UI fields. A leading zero stays decimal;
/// hexadecimal requires 0x or 0X. Source literals and addresses have their own
/// grammars. Callers trim surrounding whitespace when their UI permits it.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_NUMERICINPUT_H
#define NEVERD_SUPPORT_NUMERICINPUT_H

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace neverd {

/// Read unsigned bits without a sign, whitespace, suffix or implicit base.
inline std::optional<uint64_t>
parseUnsignedDecimalOrHex(std::string_view Text) {
  const bool Hex = Text.starts_with("0x") || Text.starts_with("0X");
  if (Hex)
    Text.remove_prefix(2);
  if (Text.empty())
    return {};
  uint64_t Value = 0;
  const auto [End, Error] = std::from_chars(
      Text.data(), Text.data() + Text.size(), Value, Hex ? 16 : 10);
  if (Error != std::errc{} || End != Text.data() + Text.size())
    return {};
  return Value;
}

/// Read a signed value, including an optional leading + or -.
inline std::optional<int64_t> parseSignedDecimalOrHex(std::string_view Text) {
  const bool Negative = Text.starts_with("-");
  if (Negative || Text.starts_with("+"))
    Text.remove_prefix(1);
  const auto Magnitude = parseUnsignedDecimalOrHex(Text);
  const auto Max = uint64_t(std::numeric_limits<int64_t>::max());
  if (!Magnitude || *Magnitude > Max + Negative)
    return {};
  if (Negative && *Magnitude == Max + 1)
    return std::numeric_limits<int64_t>::min();
  return Negative ? -int64_t(*Magnitude) : int64_t(*Magnitude);
}

} // namespace neverd

#endif // NEVERD_SUPPORT_NUMERICINPUT_H

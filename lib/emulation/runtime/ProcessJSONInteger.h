//===- ProcessJSONInteger.h - Lossless observation integers -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSJSONINTEGER_H
#define NEVERD_EMULATION_PROCESSJSONINTEGER_H

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <limits>
#include <optional>
#include <type_traits>

namespace neverd::emulation::process_json {
/// Decimal strings cover the full target width. Numeric JSON is restricted
/// to exact integers shared by C, Python and JavaScript producers.
template <typename T> std::optional<T> integer(const llvm::json::Value &Value) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
  if (auto Text = Value.getAsString()) {
    auto Digits = *Text;
    if constexpr (std::is_signed_v<T>)
      Digits.consume_front("-");
    T Number;
    if (!Digits.empty() && llvm::all_of(Digits, llvm::isDigit) &&
        !Text->getAsInteger(10, Number))
      return Number;
  } else if (auto Number = Value.getAsNumber()) {
    if (*Number >= -9007199254740991.0 && *Number <= 9007199254740991.0) {
      if constexpr (std::is_signed_v<T>) {
        auto N = Value.getAsInteger();
        if (N && *N >= std::numeric_limits<T>::min() &&
            *N <= std::numeric_limits<T>::max())
          return static_cast<T>(*N);
      } else {
        auto N = Value.getAsUINT64();
        if (N && *N <= std::numeric_limits<T>::max())
          return static_cast<T>(*N);
      }
    }
  }
  return std::nullopt;
}
} // namespace neverd::emulation::process_json
#endif

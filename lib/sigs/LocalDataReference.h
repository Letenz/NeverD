//===- LocalDataReference.h - Source-local data relations -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Identifies an object-local data operand by the external function and exact
/// operand that independently anchors it. No static spelling escapes its
/// object.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_LOCALDATAREFERENCE_H
#define NEVERD_SIGS_LOCALDATAREFERENCE_H

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::sigs {

namespace local_data_syntax {
#define NEVERD_PATTERN_CHAR(Name, Value) inline constexpr char Name = Value;
#define NEVERD_PATTERN_VALUE(Name, Value)                                      \
  inline constexpr unsigned Name = Value;
#include "neverd/sigs/PatternSyntax.def"
} // namespace local_data_syntax

struct LocalDataAnchor {
  uint32_t Offset;
  llvm::StringRef Name;
};

/// Decode the canonical eight-hex-digit offset followed by ':' and a name.
inline std::optional<LocalDataAnchor> localDataAnchor(llvm::StringRef Text) {
  constexpr size_t Digits = local_data_syntax::LocalDataAnchorOffsetDigits;
  if (Text.size() <= Digits + 1 ||
      Text[Digits] != local_data_syntax::LocalDataAnchorSeparator)
    return std::nullopt;
  const llvm::StringRef Hex = Text.take_front(Digits);
  for (char C : Hex)
    if (!((C >= '0' && C <= '9') || (C >= 'A' && C <= 'F')))
      return std::nullopt;
  uint32_t Offset;
  if (Hex.getAsInteger(local_data_syntax::Radix, Offset))
    return std::nullopt;
  const llvm::StringRef Name = Text.drop_front(Digits + 1);
  for (unsigned char C : Name)
    if (C <= ' ' || C == 0x7f)
      return std::nullopt;
  return LocalDataAnchor{Offset, Name};
}

} // namespace neverd::sigs

#endif // NEVERD_SIGS_LOCALDATAREFERENCE_H

//===- Locator.cpp - Fixed pkg loader declarations ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Streaming, ambiguity-sensitive discovery of four bounded decimal slots.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>
#include <array>

namespace neverd::web::pkg_detail {
namespace {
constexpr std::string_view Marker = "    var PAYLOAD_POSITION = '";

struct Declarations {
  std::array<uint64_t, 4> Values{};
  uint64_t Size = 0;
  bool CRLF = false;
};

Declarations read(std::string_view Text) {
  constexpr std::array<std::string_view, 4> Names{
      "PAYLOAD_POSITION", "PAYLOAD_SIZE", "PRELUDE_POSITION", "PRELUDE_SIZE"};
  const auto InitialSize = Text.size();
  auto Consume = [&](std::string_view Fixed) {
    if (!Text.starts_with(Fixed))
      throw Error("pkg_invalid_locator");
    Text.remove_prefix(Fixed.size());
  };
  Declarations D;
  for (size_t I = 0; I < Names.size(); ++I) {
    Consume("    var ");
    Consume(Names[I]);
    Consume(" = '");
    const auto Width = Names[I].size() + 6; // Original // NAME // slot.
    if (Text.size() < Width)
      throw Error("pkg_invalid_locator");
    const auto Slot = Text.substr(0, Width);
    size_t Digits = 0;
    while (Digits < Slot.size() && Slot[Digits] >= '0' && Slot[Digits] <= '9') {
      const auto Digit = uint64_t(Slot[Digits++] - '0');
      if (D.Values[I] > (MaxPkgInputBytes - Digit) / 10)
        throw Error("pkg_invalid_locator_size");
      D.Values[I] = D.Values[I] * 10 + Digit;
    }
    if (!Digits || (Digits > 1 && Slot[0] == '0') ||
        Slot.substr(Digits).find_first_not_of(' ') != Slot.npos)
      throw Error("pkg_invalid_locator_size");
    Text.remove_prefix(Width);
    Consume("' | 0;");
    if (I + 1 != Names.size()) {
      if (!I)
        D.CRLF = Text.starts_with("\r\n");
      Consume(D.CRLF ? "\r\n" : "\n");
    }
  }
  D.Size = InitialSize - Text.size();
  return D;
}
} // namespace

Layout locate(const Artifact &Input, PkgExtraction &E) {
  const auto Size = Input.Content.size();
  std::string Carry;
  uint64_t Candidates = 0;
  Declarations D;
  for (uint64_t At = 0; At < Size;) {
    const auto Count = std::min(BlobTransferBytes, Size - At);
    const auto Start = At - Carry.size();
    std::string Text = std::move(Carry);
    Text += Input.Content.read(At, Count);
    E.ScannedBytes += Count;
    const bool Last = At + Count == Size;
    size_t Search = 0;
    while (true) {
      const auto Found = Text.find(Marker, Search);
      if (Found == Text.npos)
        break;
      if (!Last &&
          Text.size() - Found < ::neverd::limits::web::kLocatorLocatorWindow)
        break;
      if (++Candidates > 1)
        throw Error("pkg_ambiguous_locator");
      D = read(std::string_view(Text).substr(
          Found, ::neverd::limits::web::kLocatorLocatorWindow));
      E.LocatorOffset = Start + Found;
      E.LocatorSize = D.Size;
      step(E, D.Size);
      Search = Found + Marker.size();
    }
    // Matches entirely before the retained suffix were already considered.
    // Keep exactly the undecided starting positions, plus a marker's overlap.
    if (!Last) {
      const auto Keep = std::min<uint64_t>(
          Text.size(), ::neverd::limits::web::kLocatorLocatorWindow - 1);
      Carry = Text.substr(Text.size() - Keep);
    }
    At += Count;
  }
  if (!Candidates)
    throw Error("pkg_locator_not_found");
  const auto [PayloadOffset, PayloadSize, PreludeOffset, PreludeSize] =
      D.Values;
  if (!PayloadOffset || PayloadOffset > Size ||
      E.LocatorOffset > PayloadOffset ||
      E.LocatorSize > PayloadOffset - E.LocatorOffset ||
      PayloadSize > Size - PayloadOffset ||
      PreludeOffset != PayloadOffset + PayloadSize || !PreludeSize ||
      PreludeSize > Size - PreludeOffset)
    throw Error("pkg_invalid_partition");
  if (PreludeSize > MaxPkgPreludeBytes)
    throw Error("pkg_prelude_budget_exceeded");
  E.PayloadOffset = PayloadOffset;
  E.PayloadSize = PayloadSize;
  E.LocatorLineStyle = D.CRLF ? "crlf" : "lf";
  return {PreludeOffset, PreludeSize};
}
} // namespace neverd::web::pkg_detail

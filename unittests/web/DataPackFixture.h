//===- DataPackFixture.h - Self-authored resource storage --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent literal v5 tables and gzip stored blocks; no producer execution.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "ZipFixture.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neverd::web::datapack_test {
inline void number(std::string &B, size_t At, uint64_t V, size_t Width) {
  for (size_t I = 0; I < Width; ++I)
    B.at(At + I) = char(V >> (8 * I));
}
inline std::string
pack(const std::vector<std::pair<uint16_t, std::string>> &Resources,
     const std::vector<std::pair<uint16_t, uint16_t>> &Aliases = {},
     uint8_t Encoding = 1) {
  const auto Start = 12 + 6 * (Resources.size() + 1) + 4 * Aliases.size();
  std::string B(Start, '\0');
  number(B, 0, 5, 4);
  number(B, 4, Encoding, 1);
  number(B, 8, Resources.size(), 2);
  number(B, 10, Aliases.size(), 2);
  uint64_t At = Start;
  for (size_t I = 0; I < Resources.size(); ++I) {
    number(B, 12 + 6 * I, Resources[I].first, 2);
    number(B, 14 + 6 * I, At, 4);
    B += Resources[I].second;
    At += Resources[I].second.size();
  }
  number(B, 14 + 6 * Resources.size(), At, 4);
  for (size_t I = 0; I < Aliases.size(); ++I) {
    const auto Offset = 12 + 6 * (Resources.size() + 1) + 4 * I;
    number(B, Offset, Aliases[I].first, 2);
    number(B, Offset + 2, Aliases[I].second, 2);
  }
  return B;
}
inline std::string
widePack(const std::vector<std::pair<uint32_t, std::string>> &Resources,
         const std::vector<std::pair<uint32_t, uint32_t>> &Aliases = {}) {
  const auto Start = 16 + 8 * (Resources.size() + 1) + 8 * Aliases.size();
  std::string B(Start, '\0');
  number(B, 0, 5, 4);
  number(B, 4, 1, 4);
  number(B, 8, Resources.size(), 4);
  number(B, 12, Aliases.size(), 4);
  uint64_t At = Start;
  for (size_t I = 0; I < Resources.size(); ++I) {
    number(B, 16 + 8 * I, Resources[I].first, 4);
    number(B, 20 + 8 * I, At, 4);
    B += Resources[I].second;
    At += Resources[I].second.size();
  }
  number(B, 20 + 8 * Resources.size(), At, 4);
  for (size_t I = 0; I < Aliases.size(); ++I) {
    const auto Offset = 16 + 8 * (Resources.size() + 1) + 8 * I;
    number(B, Offset, Aliases[I].first, 4);
    number(B, Offset + 4, Aliases[I].second, 4);
  }
  return B;
}
inline std::string gzipStored(std::string_view Text) {
  if (Text.size() > 65535)
    throw std::runtime_error("fixture stored block is too large");
  const auto Size = uint16_t(Text.size());
  std::string B("\x1f\x8b\x08\0\0\0\0\0\0\x03\x01", 11);
  B.resize(15);
  number(B, 11, Size, 2);
  number(B, 13, uint16_t(~Size), 2);
  B += Text;
  const auto At = B.size();
  B.resize(At + 8);
  number(B, At, test::zipCRC(Text), 4);
  number(B, At + 4, Text.size(), 4);
  return B;
}
} // namespace neverd::web::datapack_test

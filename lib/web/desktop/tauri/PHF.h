//===- PHF.h - PHF 0.10 string lookup validation -----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SipHash13-128 words and wrapping displacement arithmetic, not a digest.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string_view>

namespace neverd::web::tauri_detail {
struct PHFHash {
  uint32_t G, F1, F2;
};
struct SipWords {
  uint64_t Lower, Upper;
};
SipWords sip13_128(std::string_view Key, uint64_t Key0, uint64_t Key1);
PHFHash phfHash(std::string_view Key, uint64_t Seed);
uint32_t phfIndex(PHFHash Hash, uint32_t D1, uint32_t D2, uint32_t Count);
} // namespace neverd::web::tauri_detail

//===- PHF.cpp - SipHash13-128 and PHF string lookup ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PHF 0.10 string hashing. Algorithm adapted from rust-siphash 0.3.10 and
/// rust-phf 0.10.1; original notices and exact sources are in LICENSES/phf.
/// Modified 2026-10-12: bounded one-shot C++ projection of byte string keys.
///
//===----------------------------------------------------------------------===//
// Copyright 2012-2015 The Rust Project Developers.
// Copyright 2012-2016 The Rust Project Developers.
// Copyright 2016-2021 Frank Denis.
// Copyright (c) 2014-2016 Steven Fackler.
// Licensed under the Apache License, Version 2.0 or the MIT license, at your
// option. See LICENSES/phf for both licenses and the PHF author notice.

#include "PHF.h"

#include <bit>

namespace neverd::web::tauri_detail {
namespace {
struct SipState {
  uint64_t A = 0x736f6d6570736575ULL, B, C = 0x6c7967656e657261ULL, D;
  SipState(uint64_t Key0, uint64_t Key1)
      : B(Key1 ^ 0x646f72616e646f83ULL), D(Key1 ^ 0x7465646279746573ULL) {
    A ^= Key0;
    C ^= Key0;
  }
  void round() {
    A += B;
    B = std::rotl(B, 13) ^ A;
    A = std::rotl(A, 32);
    C += D;
    D = std::rotl(D, 16) ^ C;
    A += D;
    D = std::rotl(D, 21) ^ A;
    C += B;
    B = std::rotl(B, 17) ^ C;
    C = std::rotl(C, 32);
  }
  void word(uint64_t W) {
    D ^= W;
    round();
    A ^= W;
  }
  uint64_t final() {
    round();
    round();
    round();
    return A ^ B ^ C ^ D;
  }
};
} // namespace
SipWords sip13_128(std::string_view Key, uint64_t Key0, uint64_t Key1) {
  SipState S(Key0, Key1);
  uint64_t Tail = uint64_t(Key.size() & 255) << 56;
  size_t At = 0;
  while (Key.size() - At >= 8) {
    uint64_t W = 0;
    for (unsigned I = 0; I < 8; ++I)
      W |= uint64_t(uint8_t(Key[At++])) << (8 * I);
    S.word(W);
  }
  unsigned Shift = 0;
  while (At < Key.size()) {
    Tail |= uint64_t(uint8_t(Key[At++])) << Shift;
    Shift += 8;
  }
  S.word(Tail);
  S.C ^= 0xee;
  const auto Lower = S.final();
  S.B ^= 0xdd;
  const auto Upper = S.final();
  return {Lower, Upper};
}
PHFHash phfHash(std::string_view Key, uint64_t Seed) {
  const auto H = sip13_128(Key, 0, Seed);
  return {uint32_t(H.Lower >> 32), uint32_t(H.Lower), uint32_t(H.Upper)};
}
uint32_t phfIndex(PHFHash H, uint32_t D1, uint32_t D2, uint32_t Count) {
  return uint32_t(D2 + H.F1 * D1 + H.F2) % Count;
}
} // namespace neverd::web::tauri_detail

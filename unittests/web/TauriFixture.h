//===- TauriFixture.h - Authored inert PHF and Brotli assets -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent stored bytes and native x64 instruction encodings. Compressed
/// known answers were recorded with the native 1.2.0 encoder during
/// development; fixtures and tests never invoke a codec tool, compiler or
/// target runtime.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "NativeFixture.h"

#include <array>

namespace neverd::web::tauri_test {
inline constexpr uint64_t Base = 0x100000000, Start = Base + 256;
inline constexpr uint64_t Disps = 512, Rows = 544;
inline std::string javascript() { return "export const CANARY = 7;\n"; }
inline std::string html() {
  return "<!doctype html><script src=\"/entry.js\"></script>";
}
inline std::string compressedJS() {
  const uint8_t Bytes[]{0x0b, 0x0c, 0x80, 0x65, 0x78, 0x70, 0x6f, 0x72,
                        0x74, 0x20, 0x63, 0x6f, 0x6e, 0x73, 0x74, 0x20,
                        0x43, 0x41, 0x4e, 0x41, 0x52, 0x59, 0x20, 0x3d,
                        0x20, 0x37, 0x3b, 0x0a, 0x03};
  return {reinterpret_cast<const char *>(Bytes), sizeof(Bytes)};
}
inline std::string compressedHTML() {
  const uint8_t Bytes[]{0x1b, 0x2f, 0x00, 0x00, 0xa4, 0x5e, 0x78, 0x52,
                        0x98, 0x46, 0x16, 0xe9, 0x49, 0x03, 0x55, 0x82,
                        0x87, 0x07, 0xff, 0x01, 0x11, 0x86, 0x04};
  return {reinterpret_cast<const char *>(Bytes), sizeof(Bytes)};
}
inline std::string image() {
  auto B = test::nativeMachO();
  B.resize(4096, '\0');
  auto Put = [&](uint64_t At, uint64_t Value, unsigned Width = 8) {
    test::nativePut(B, At, Value, Width);
  };
  Put(80, B.size());
  Put(144, 53);
  // LC_FUNCTION_STARTS records the selected entry. A prologue-looking byte
  // sequence or LC_MAIN alone is not a native function-boundary witness.
  Put(16, 3, 4);
  Put(20, 192, 4);
  Put(208, 0x26, 4);
  Put(212, 16, 4);
  Put(216, 960, 4);
  Put(220, 3, 4);
  B.replace(960, 3, "\x80\x02\0", 3);
  const uint8_t Code[]{
      0x48, 0x8d, 0x05, 0xf9, 0, 0, 0, // lea rax, [rip+249] -> disps
      0x49, 0x89, 0x45, 0x10, 0x49, 0xc7, 0x45, 0x18, 1, 0, 0, 0, 0x48, 0x8d,
      0x05, 0x06, 1, 0, 0, // lea rax, [rip+262]
                           // -> rows
      0x49, 0x89, 0x45, 0x20, 0x49, 0xc7, 0x45, 0x28, 2, 0, 0, 0, 0x48, 0xb8, 0,
      0, 0, 0, 0, 0, 0, 0, 0x49, 0x89, 0x45, 0x30, 0xc3};
  B.replace(256, sizeof(Code), reinterpret_cast<const char *>(Code),
            sizeof(Code));
  // A fixed one-bucket PHF known answer for seed zero. Validate against both
  // row keys; this fixture does not call the production table generator.
  Put(Disps, 0, 4);
  Put(Disps + 4, 1, 4);
  const std::array<std::string, 2> Keys{"/entry.js", "/index.html"};
  const std::array<std::string, 2> Payloads{compressedJS(), compressedHTML()};
  uint64_t At = 640;
  for (size_t I = 0; I < 2; ++I) {
    const auto Row = Rows + I * 32;
    Put(Row, Base + At);
    Put(Row + 8, Keys[I].size());
    B.replace(At, Keys[I].size(), Keys[I]);
    At += Keys[I].size();
    Put(Row + 16, Base + At);
    Put(Row + 24, Payloads[I].size());
    B.replace(At, Payloads[I].size(), Payloads[I]);
    At += Payloads[I].size();
  }
  return B;
}
} // namespace neverd::web::tauri_test

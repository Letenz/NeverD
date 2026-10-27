//===- DarwinLibSystemMemoryTestData.h - Imported mappings ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TESTS_DARWINLIBSYSTEMMEMORYTESTDATA_H
#define NEVERD_TESTS_DARWINLIBSYSTEMMEMORYTESTDATA_H

#include "neverd/emulation/DarwinFileOptions.h"

namespace neverd::emulation::darwin_test {
inline DarwinFileOptions libSystemMemoryOptions(uint64_t Page) {
  DarwinFileOptions O;
  auto &Bytes = O.Files["/data"];
  Bytes.assign(Page, 'x');
  Bytes.insert(Bytes.end(), {'f', 0, 'l'});
  return O;
}
struct LibSystemMemoryCall {
  const char *Name;
  unsigned Count;
};
inline constexpr LibSystemMemoryCall LibSystemMemoryCalls[] = {
    {"___error", 0},  {"_mmap", 6},     {"_mmap", 6},     {"_mmap", 6},
    {"_mprotect", 3}, {"_mprotect", 3}, {"_mprotect", 3}, {"_write", 3},
    {"_munmap", 2},   {"_mmap", 6},     {"_mmap", 6},     {"_open", 2},
    {"_mmap", 6},     {"_close", 1},    {"_open", 2},     {"_pread", 4},
    {"_close", 1},    {"_munmap", 2}};
} // namespace neverd::emulation::darwin_test
#endif

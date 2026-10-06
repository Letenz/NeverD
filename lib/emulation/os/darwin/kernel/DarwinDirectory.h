//===- DarwinDirectory.h - Darwin directory record sizing -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_DARWIN_DARWINDIRECTORY_H
#define NEVERD_EMULATION_OS_DARWIN_DARWINDIRECTORY_H

#include <cstdint>

namespace neverd::emulation::darwin_model {
inline constexpr uint64_t DirectoryPayloadLimit = 128 * 1024 * 1024;
inline constexpr uint64_t DirectoryExtendedMinimum = 1024;
inline constexpr uint64_t DirectoryFlagsSize = 4;
inline constexpr uint64_t DirectoryNameOffset = 21;
// LP64 records include the native struct's trailing padding, not merely a
// terminated name. Names have already been bounded by catalogue admission.
inline constexpr uint64_t directoryRecordSize(uint64_t NameBytes) {
  return (25 + NameBytes + 7) & ~uint64_t(7);
}
} // namespace neverd::emulation::darwin_model
#endif

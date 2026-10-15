//===- AndroidProperties.h - Immutable Android property image --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One explicit property table owns name lookup, stable handles and bytes.

#ifndef NEVERD_EMULATION_ANDROIDPROPERTIES_H
#define NEVERD_EMULATION_ANDROIDPROPERTIES_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace neverd::emulation::android_model {
namespace property_abi {
inline constexpr uint64_t ValueOffset = 4, ValueBytes = 92, NameOffset = 96;
inline constexpr uint64_t LegacyNameBytes = 32;
// Placement is model policy, independent of Android's property-area layout.
// Keep this arena disjoint from the image, TLS, thunks and thread arenas.
inline constexpr uint64_t Base = 0x7010000000, Capacity = 0x10000000;
} // namespace property_abi

struct PropertyRecord {
  uint64_t Address;
  std::string Name, Value;
  uint32_t Serial;
  std::vector<uint8_t> Bytes;
};
struct PropertyTable {
  // Address order also defines deterministic enumeration of this snapshot.
  std::vector<PropertyRecord> Records;
  std::map<std::string, size_t> Names;
};
} // namespace neverd::emulation::android_model
#endif

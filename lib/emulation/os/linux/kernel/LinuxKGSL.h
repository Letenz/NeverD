//===- LinuxKGSL.h - Explicit LP64 context ABI facts ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authoritative LP64 context, timestamp and admitted command request facts.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXKGSL_H
#define NEVERD_EMULATION_OS_LINUX_LINUXKGSL_H
#include "neverd/emulation/LinuxDeviceOptions.h"

#include <optional>

namespace neverd::emulation::linux_model::kgsl {
inline constexpr uint32_t Create = 0xc0080913;
inline constexpr uint32_t Destroy = 0x40040914;
inline constexpr uint32_t ReadTimestamp = 0xc00c0916;
inline constexpr uint32_t MapUserMemory = 0xc0300915;
inline constexpr uint32_t FreeMemory = 0x40080921;
inline constexpr uint32_t SubmitCommands = 0xc0200910;
inline constexpr uint32_t WaitTimestamp = 0x400c0907;
inline constexpr uint32_t AllocateObject = 0xc0300945;
inline constexpr uint32_t FreeObject = 0x40200946;
inline constexpr uint32_t ObjectInfo = 0xc0300947;
inline bool isObjectRequest(uint32_t Request) {
  return Request == AllocateObject || Request == FreeObject ||
         Request == ObjectInfo;
}
inline constexpr uint64_t GPUReadOnly = 0x01000000;
inline constexpr uint64_t MaximumAllocation = 32768;
inline constexpr uint64_t MaximumAllocations = 64;
/// Deterministic GPU VA policy, excluding explicit legacy device ranges.
inline std::optional<uint64_t>
allocationAddress(const LinuxDeviceOptions &Device, uint64_t Start,
                  uint64_t Size) {
  if (Size > UINT32_MAX)
    return std::nullopt;
  const uint64_t Extent = Size + 4096;
  for (;;) {
    if (Start >= 0x100000000ULL || Extent > 0x100000000ULL - Start)
      return std::nullopt;
    bool Collision = false;
    for (const auto &Mapping : Device.ReadOnlyMappings)
      if (Start < Mapping.Offset + Mapping.Bytes.size() &&
          Mapping.Offset < Start + Extent) {
        Start = Mapping.Offset + Mapping.Bytes.size();
        Collision = true;
        break;
      }
    if (!Collision)
      return Start;
  }
}
inline constexpr uint32_t SlotBytes = 40;
inline constexpr uint32_t MaximumContexts = 1024;
inline constexpr uint32_t CreateFlags = 0x12; // PREAMBLE | NO_GMEM_ALLOC
inline constexpr uint32_t ReturnedFlags =
    0x8052; // per-context TS, medium priority
inline bool isContextRequest(uint32_t Request) {
  return Request == Create || Request == Destroy || Request == ReadTimestamp;
}
inline bool isCommandRequest(uint32_t Request) {
  return Request == MapUserMemory || Request == FreeMemory ||
         Request == SubmitCommands || Request == WaitTimestamp;
}
inline const LinuxDeviceMapping *memstore(const LinuxDeviceOptions &Device) {
  if (Device.KGSL)
    for (const auto &Mapping : Device.ReadOnlyMappings)
      if (Mapping.Offset == Device.KGSL->MemstoreOffset)
        return &Mapping;
  return nullptr;
}
} // namespace neverd::emulation::linux_model::kgsl
#endif

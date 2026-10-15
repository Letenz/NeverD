//===- LinuxKGSL.h - Explicit LP64 context ABI facts ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Narrow context/timestamp protocol, independent of command execution.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXKGSL_H
#define NEVERD_EMULATION_OS_LINUX_LINUXKGSL_H
#include "neverd/emulation/LinuxDeviceOptions.h"

namespace neverd::emulation::linux_model::kgsl {
inline constexpr uint32_t Create = 0xc0080913;
inline constexpr uint32_t Destroy = 0x40040914;
inline constexpr uint32_t ReadTimestamp = 0xc00c0916;
inline constexpr uint32_t SlotBytes = 40;
inline constexpr uint32_t MaximumContexts = 1024;
inline constexpr uint32_t CreateFlags = 0x12; // PREAMBLE | NO_GMEM_ALLOC
inline constexpr uint32_t ReturnedFlags =
    0x8052; // per-context TS, medium priority
inline bool isContextRequest(uint32_t Request) {
  return Request == Create || Request == Destroy || Request == ReadTimestamp;
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

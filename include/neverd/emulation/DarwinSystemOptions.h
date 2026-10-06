//===- DarwinSystemOptions.h - Explicit system observations -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H
#define NEVERD_EMULATION_DARWINSYSTEMOPTIONS_H

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::emulation {
/// Fixed sysctl observations, independent of host hardware and OS identity.
/// Missing is unknown; an empty string is an explicit value. Strings contain
/// no NUL and at most 1023 bytes. Page size comes from the guest memory policy.
struct DarwinSystemOptions {
  std::optional<std::string> OSType;
  std::optional<std::string> OSRelease;
  std::optional<std::string> OSVersion;
  std::optional<std::string> KernelVersion;
  std::optional<std::string> Machine;
  std::optional<std::string> Model;
  std::optional<int32_t> OSRevision;
  /// Positive, at most INT32_MAX. This does not add thread scheduling.
  std::optional<uint32_t> CPUCount;
  /// Reported memory, independent of the emulation allocation budget.
  std::optional<uint64_t> MemorySize;
};
} // namespace neverd::emulation
#endif

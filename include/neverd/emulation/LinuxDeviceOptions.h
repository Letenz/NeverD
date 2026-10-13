//===- LinuxDeviceOptions.h - Explicit device transaction inputs -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded device observations, independent of a host driver or device ABI.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXDEVICEOPTIONS_H
#define NEVERD_EMULATION_LINUXDEVICEOPTIONS_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::emulation {
struct LinuxDeviceMemory {
  /// Relative to ioctl's argument, or to the little-endian guest pointer
  /// read at argument + PointerOffset when that offset is present.
  uint64_t Offset = 0;
  std::optional<uint64_t> PointerOffset;
  std::vector<uint8_t> Expected;
  std::vector<uint8_t> Output;
};

struct LinuxDeviceIoctl {
  uint32_t Request = 0;
  int64_t Result = 0;
  std::optional<uint64_t> Argument;
  std::vector<LinuxDeviceMemory> Memory;
};

struct LinuxDeviceOptions {
  /// Exact admitted O_ACCMODE (0, 1 or 2). No creation or truncation contract.
  uint32_t AccessMode = 0;
  /// Explicit open outcome: zero admits an owned descriptor, else guest errno.
  uint32_t OpenErrno = 0;
  /// Ordered, single-use transactions for each independent open description.
  /// An absent/mismatched next transaction stops instead of inventing a reply.
  std::vector<LinuxDeviceIoctl> Ioctls;
};
} // namespace neverd::emulation
#endif

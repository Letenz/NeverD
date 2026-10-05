//===- LinuxFileOptions.h - Explicit memory file inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXFILEOPTIONS_H
#define NEVERD_EMULATION_LINUXFILEOPTIONS_H

#include "neverd/emulation/LinuxTimeOptions.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace neverd::emulation {
/// Fixed observations for a regular memory file, independent of its bytes.
/// Device uses Linux's encoded 32-bit device number. No host state is read.
struct LinuxFileMetadata {
  uint32_t Device = 0;
  uint64_t Inode = 0;
  uint32_t Mode = 0;
  uint32_t LinkCount = 0;
  uint32_t UID = 0;
  uint32_t GID = 0;
  uint64_t Size = 0;
  uint32_t BlockSize = 0;
  uint64_t Blocks = 0;
  LinuxTimespec AccessTime;
  LinuxTimespec ModificationTime;
  LinuxTimespec ChangeTime;
};

/// Closed, immutable file catalogue for Linux ELF64 and Android workloads.
/// Keys are canonical absolute guest paths, never host paths. Each open has
/// its own cursor; all guest threads share the workload's descriptor table.
struct LinuxFileOptions {
  std::map<std::string, std::vector<uint8_t>> Files;
  /// Exclusive descriptor ceiling, including initially reserved 0, 1 and 2.
  uint32_t DescriptorLimit = 256;
  /// Optional status observations keyed by an existing Files path. Absent
  /// metadata is unknown. These observations do not change catalogue access.
  std::map<std::string, LinuxFileMetadata> Metadata;
};
} // namespace neverd::emulation
#endif

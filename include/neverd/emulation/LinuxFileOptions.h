//===- LinuxFileOptions.h - Explicit memory file inputs --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXFILEOPTIONS_H
#define NEVERD_EMULATION_LINUXFILEOPTIONS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace neverd::emulation {
/// Closed, immutable file catalogue for Linux ELF64 and Android workloads.
/// Keys are canonical absolute guest paths, never host paths. Each open has
/// its own cursor; all guest threads share the workload's descriptor table.
struct LinuxFileOptions {
  std::map<std::string, std::vector<uint8_t>> Files;
  /// Exclusive descriptor ceiling, including initially reserved 0, 1 and 2.
  uint32_t DescriptorLimit = 256;
};
} // namespace neverd::emulation
#endif

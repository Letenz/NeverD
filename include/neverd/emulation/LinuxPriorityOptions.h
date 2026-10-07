//===- LinuxPriorityOptions.h - Explicit guest task priorities --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_LINUXPRIORITYOPTIONS_H
#define NEVERD_EMULATION_LINUXPRIORITYOPTIONS_H

#include <cstdint>
#include <map>

namespace neverd::emulation {
/// Explicit initial nice values of fixture-owned tasks with the caller's UID.
/// Unlisted task identities and group/user selection remain unmodeled.
struct LinuxPriorityOptions {
  std::map<uint32_t, int32_t> Tasks;
  bool CapSysNice = false;
  uint32_t RlimitNice = 0;
};
} // namespace neverd::emulation
#endif

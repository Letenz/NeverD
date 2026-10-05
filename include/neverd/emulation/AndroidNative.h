//===- AndroidNative.h - Explicit Android native call inputs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDNATIVE_H
#define NEVERD_EMULATION_ANDROIDNATIVE_H

#include "neverd/emulation/ProcessCall.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct NativeMemoryRegion {
  uint64_t Address = 0, Size = 0;
  /// Initial bytes followed by zero padding. Address and Size are page aligned.
  std::vector<uint8_t> Bytes;
  bool Executable = false;
  /// Copy this explicit regular file into the region before execution, then
  /// zero pad. Mutually exclusive with Bytes; guest writes never modify it.
  /// Relative paths use the caller's working directory. The file must fit Size.
  std::optional<std::filesystem::path> File;
};
struct NativeMemoryRead {
  uint64_t Address = 0, Size = 0;
  /// Require a readable mapping before guest execution. False permits memory
  /// allocated during the call; the complete range must be readable at the
  /// final resumable stop. Neither setting creates or preserves mappings.
  bool RequireMappedAtEntry = true;
};
struct NativeMemorySnapshot {
  uint64_t Address;
  std::vector<uint8_t> Bytes;
};

/// Android 9 / API 28, little-endian AArch64. Function
/// arguments are AAPCS64 scalar register/stack values, not process argv.
/// Explicit inputs never inherit host properties, files, or environment.
struct AndroidNativeOptions {
  uint64_t LoadBias = 0x40000000;
  /// Select exactly one exported symbol or link-time virtual address.
  std::string EntrySymbol;
  std::optional<uint64_t> EntryAddress;
  std::vector<uint64_t> Arguments;
  std::map<std::string, std::string> Properties;
  std::vector<NativeMemoryRegion> Memory;
  std::vector<NativeMemoryRead> ReadMemory;
  /// Constructors normally execute before the call, sharing its budget.
  /// False explicitly requests analysis of an uninitialized library.
  bool Initialize = true;
  /// Retain this many admitted instruction PCs. Zero disables tracing.
  uint64_t TraceLimit = 0;
  /// One preserves the single-thread contract. Values 2..256 opt into bounded
  /// cooperative guest threads. Counts all created identities, including the
  /// entry thread; retired identities are never reused within a workload.
  uint64_t ThreadLimit = 1;
  /// Continue scheduling children after the selected entry returns. Otherwise
  /// stop at that return and report the remaining thread states. Requires
  /// ThreadLimit > 1; never replenishes execution limits.
  bool DrainThreads = false;
  /// Exact library names and available function names for the local dlfcn
  /// model. No host files are loaded. Calling an unmodeled function still
  /// stops.
  std::map<std::string, std::vector<std::string>> Libraries;
  /// Complete ordered RTLD_DEFAULT search scope for this workload. Entries
  /// name resident providers in Libraries; dlopen/close do not change this
  /// scope or unload them. Absent means unknown (unsupported); empty means
  /// explicitly no providers. No namespace or dependency order is inferred.
  std::optional<std::vector<std::string>> DefaultScope;
};

/// Model-owned identity and placement, not a public pthread_internal_t layout.
struct NativeThreadSnapshot {
  uint64_t ID, Handle, TLS, StackBase, StackSize, GuardSize;
  bool Finished = false, Detached = false, Retired = false, Waiting = false;
  std::optional<uint64_t> ReturnValue;
};
struct NativeTraceThread {
  uint64_t Index, ID;
};
} // namespace neverd::emulation
#endif

//===- AndroidNative.h - Explicit Android native call inputs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDNATIVE_H
#define NEVERD_EMULATION_ANDROIDNATIVE_H

#include <array>
#include <cstdint>
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
};
struct NativeMemoryRead {
  uint64_t Address = 0, Size = 0;
};
struct NativeMemorySnapshot {
  uint64_t Address;
  std::vector<uint8_t> Bytes;
};
struct NativeCallEvent {
  uint64_t PC;
  std::string Name;
  std::array<uint64_t, 8> Arguments{};
  std::optional<uint64_t> Result;
};
/// Android 9 / API 28, little-endian AArch64, one native thread. Function
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
};
} // namespace neverd::emulation
#endif

//===- DarwinProcessOptions.h - Explicit Darwin image inputs ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Bounded startup dylib catalogue; no host path search.
#ifndef NEVERD_EMULATION_DARWINPROCESSOPTIONS_H
#define NEVERD_EMULATION_DARWINPROCESSOPTIONS_H
#include <filesystem>
#include <string>
#include <vector>
namespace neverd::emulation {
inline constexpr unsigned DarwinModuleLimit = 32;
inline constexpr unsigned DarwinModuleNameLimit = 4096;
struct DarwinModuleInput {
  /// Exact case-sensitive install name, including any literal @ prefix.
  std::string Name;
  /// Original Mach-O bytes, never opened as a host library.
  std::filesystem::path Path;
};
struct DarwinProcessOptions {
  /// Only reachable startup dependencies are read; libSystem is reserved.
  std::vector<DarwinModuleInput> Modules;
};
} // namespace neverd::emulation
#endif

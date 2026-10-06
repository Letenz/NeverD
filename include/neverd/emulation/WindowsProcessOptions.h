//===- WindowsProcessOptions.h - Explicit Windows inputs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWSPROCESSOPTIONS_H
#define NEVERD_EMULATION_WINDOWSPROCESSOPTIONS_H
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neverd::emulation {
namespace windows_process_limits {
#define NEVERD_WINDOWS_OPTION_LIMIT(Name, Value)                               \
  inline constexpr uint64_t Name = Value;
#include "neverd/emulation/WindowsProcessOptions.def"
#undef NEVERD_WINDOWS_OPTION_LIMIT
} // namespace windows_process_limits
struct WindowsModuleInput {
  /// Exact guest basename, compared without ASCII case. No guest search path.
  std::string Name;
  /// Explicit host input file, never loaded as host executable code.
  std::filesystem::path Path;
};
struct WindowsProcessOptions {
  /// Only reachable startup dependencies are read. API providers cannot be
  /// overridden. DLL initialization and dynamic loading are separate contracts.
  std::vector<WindowsModuleInput> Modules;
  /// Load an image whose loader facts the model does not implement, and stop
  /// only if execution depends on one. Exports outside the API inventory and
  /// modules outside the catalogue bind to opaque entries: they resolve to
  /// distinct addresses, and executing one stops as an unsupported service
  /// that names it. Directories the model does not interpret stay
  /// uninterpreted, metadata the file does not back is not read at load, and
  /// frame-based exception dispatch through such an image stops. Nothing is
  /// skipped or answered on behalf of an unmodeled behavior.
  bool DeferUnmodeled = false;
};
} // namespace neverd::emulation
#endif

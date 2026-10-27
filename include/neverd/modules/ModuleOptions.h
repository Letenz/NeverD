//===- ModuleOptions.h - Explicit binary and debug inputs -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit binary and debug inputs.
///

#ifndef NEVERD_MODULES_MODULEOPTIONS_H
#define NEVERD_MODULES_MODULEOPTIONS_H

#include "neverd/modules/Limits.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd::modules {

enum class LoadPolicy { Eager, Lazy };
enum class DebugKind { PDB, DWARF, Map };

const char *loadPolicyName(LoadPolicy Policy);
const char *debugKindName(DebugKind Kind);

struct DebugInput {
  DebugKind Kind = DebugKind::PDB;
  std::filesystem::path Path;
};

struct ModuleInput {
  /// Caller identity, case sensitive and independent of any guest module name.
  std::string ID;
  std::filesystem::path Image;
  std::optional<DebugInput> Debug;
  /// Omission inherits ModuleOptions::DefaultLoad.
  std::optional<LoadPolicy> Load;
};

struct ModuleLimits {
  uint64_t Modules = 256;
  uint64_t ImageBytes = 256ULL << 20;
  uint64_t DebugBytes = 512ULL << 20;
  /// Retained source bytes, not a claim about all parser/cache allocations.
  uint64_t TotalSourceBytes = 1ULL << 30;
  uint64_t Symbols = 1000000;
};

struct ModuleOptions {
  /// Explicit origin for relative inputs. No cwd or host-system fallback.
  std::filesystem::path BaseDirectory;
  LoadPolicy DefaultLoad = LoadPolicy::Eager;
  ModuleLimits Limits;
  std::vector<ModuleInput> Modules;
};

/// Shared path admission for scene-owned file inputs as well as modules.
/// Never searches the current directory or host-system paths.
llvm::Expected<std::filesystem::path>
normalizeInputPath(llvm::StringRef,
                   const std::filesystem::path &BaseDirectory = {});
/// Validate without opening any input. Resolves paths against the explicit
/// absolute base, retaining declarations even when lazy files are absent.
llvm::Expected<ModuleOptions> normalizeModuleOptions(ModuleOptions Options);
llvm::Expected<ModuleOptions>
moduleOptionsFromJSON(llvm::StringRef JSON,
                      const std::filesystem::path &BaseDirectory = {});
/// Read only this explicitly named regular configuration file. An explicit
/// base_directory in its JSON wins; otherwise relative inputs use the file's
/// absolute parent directory. No module binary/debug file is opened here.
llvm::Expected<ModuleOptions>
moduleOptionsFromFile(const std::filesystem::path &Configuration);

} // namespace neverd::modules
#endif

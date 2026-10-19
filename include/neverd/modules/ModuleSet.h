//===- ModuleSet.h - Incremental binary resource catalogue ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Incremental binary resource catalogue.
///

#ifndef NEVERD_MODULES_MODULESET_H
#define NEVERD_MODULES_MODULESET_H

#include "neverd/modules/ModuleImage.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace neverd::modules {

struct ModuleInfo {
  ModuleInput Input;
  LoadPolicy Load = LoadPolicy::Eager;
  bool Prepared = false;
  std::string ImageSHA256, DebugSHA256;
  uint64_t SourceBytes = 0;
};

/// Registration performs no file IO, even for eager entries. A consumer calls
/// prepareEager before execution and prepare when a lazy resource is needed.
/// Published observations remain alive when this catalogue changes or dies.
/// Operations require external serialization; adding a resource never changes
/// any execution's mapping or implicitly resumes a CPU.
class ModuleSet final {
public:
  ModuleSet(const ModuleSet &) = delete;
  ModuleSet &operator=(const ModuleSet &) = delete;
  static llvm::Expected<std::unique_ptr<ModuleSet>> create(ModuleOptions);
  static llvm::Expected<std::unique_ptr<ModuleSet>>
  fromJSON(llvm::StringRef JSON,
           const std::filesystem::path &BaseDirectory = {});

  llvm::Error add(ModuleInput);
  /// Correct an input that has never published an observation, retaining its
  /// ID/load policy. No IO or execution mutation; prepared inputs stay fixed.
  llvm::Error reviseUnprepared(llvm::StringRef ID,
                               const std::filesystem::path &Image,
                               std::optional<DebugInput> Debug = {});
  llvm::Error prepareEager();
  llvm::Expected<std::shared_ptr<const ModuleImage>>
  prepare(llvm::StringRef ID);
  llvm::Expected<std::shared_ptr<const ModuleImage>>
  attachDebug(llvm::StringRef ID, DebugInput);
  std::shared_ptr<const ModuleImage> prepared(llvm::StringRef ID) const;
  std::vector<ModuleInfo> inspect() const;
  llvm::Expected<std::optional<ModuleSymbol>>
  lookupSymbol(llvm::StringRef ID, llvm::StringRef Name);
  uint64_t revision() const { return Revision; }
  uint64_t retainedSourceBytes() const { return SourceBytes; }

private:
  struct Entry {
    ModuleInput Input;
    std::shared_ptr<const ModuleImage> Image;
  };
  explicit ModuleSet(ModuleOptions Options) : Options(std::move(Options)) {}
  ModuleOptions Options;
  std::map<std::string, Entry> Entries;
  uint64_t SourceBytes = 0, Revision = 0;
};

std::string moduleSetJSON(const ModuleSet &Modules);

} // namespace neverd::modules
#endif

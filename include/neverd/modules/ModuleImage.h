//===- ModuleImage.h - Immutable module observations ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable module observations.
///

#ifndef NEVERD_MODULES_MODULEIMAGE_H
#define NEVERD_MODULES_MODULEIMAGE_H

#include "neverd/modules/ModuleOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <optional>
#include <string>

namespace neverd {
struct BinaryImage;
class DebugContext;
namespace modules {

enum class SymbolKind { Function, Data };
struct ModuleSymbol {
  std::string Name;
  /// Original image address, never an implicitly selected guest address.
  uint64_t Address = 0;
  uint64_t Size = 0;
  SymbolKind Kind = SymbolKind::Function;
};

/// Shared file/symbol observation. It contains no process, guest mapping,
/// relocation bias, OS policy or execution state. Query/cache access requires
/// caller serialization, just like the underlying DebugContext providers.
class ModuleImage final {
public:
  ~ModuleImage();
  const BinaryImage &image() const;
  const DebugContext *debug() const;
  llvm::ArrayRef<uint8_t> bytes() const;
  const std::string &imageDigest() const;
  const std::string &debugDigest() const;
  const std::filesystem::path &sourcePath() const;
  const std::filesystem::path &debugSourcePath() const;
  /// Owned, read-only source copies for existing path-based loaders. They
  /// remain alive with this observation and are never executed on the host.
  const std::filesystem::path &preparedImagePath() const;
  const std::filesystem::path &preparedDebugPath() const;
  uint64_t sourceBytes() const;
  std::optional<DebugKind> debugKind() const;
  bool hasAuthenticatedFunctionSignatures() const;
  bool hasAuthenticatedObjectExtents() const;

  /// Equal-name symbols at distinct addresses/kinds are an error, not a
  /// first-wins binding. MAP names never acquire authenticated semantics.
  llvm::Expected<std::optional<ModuleSymbol>>
  lookupSymbol(llvm::StringRef Name) const;

private:
  friend class ModuleSet;
  struct Storage;
  explicit ModuleImage(std::unique_ptr<Storage> Data);
  static llvm::Expected<std::shared_ptr<const ModuleImage>>
  load(const ModuleInput &, const ModuleLimits &, uint64_t AvailableBytes);
  static llvm::Expected<std::shared_ptr<const ModuleImage>>
  withDebug(const ModuleImage &, const DebugInput &, const ModuleLimits &,
            uint64_t AvailableBytes);
  std::unique_ptr<Storage> Data;
};

} // namespace modules
} // namespace neverd
#endif

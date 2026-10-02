//===- WindowsProcessModules.h - Guest startup linking ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_MODULES_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_MODULES_H
#include "WindowsProcess.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace neverd::emulation::windows_process {
struct Module {
  Image Loaded;
  /// Index original entries, retaining holes, aliases and unresolved
  /// forwarders.
  std::map<std::string, size_t> Names;
  std::map<uint32_t, size_t> Ordinals;
  std::vector<PEMetadataRange> ExportMetadata;
};
struct Program {
  std::vector<Module> Modules;
  std::vector<ModuleIdentity> Identities;
  std::vector<size_t> InitializationOrder;
  /// One exact provider/name gate per process, independent of the caller image.
  std::vector<Import> Gates;
  std::map<std::pair<std::string, std::string>, uint64_t> ServiceGates;
  /// Preparation and subsequent export queries never replenish these credits.
  ImageReadBudget Reads{0, 0};
};
llvm::Expected<std::string> moduleName(llvm::StringRef Name);
using ForwardModule =
    llvm::function_ref<llvm::Expected<size_t>(size_t, llvm::StringRef)>;
/// Resolve one demanded identity. A missing export is distinct from a malformed
/// chain or an unsupported runtime module load. CPU enables live metadata
/// checks.
llvm::Expected<std::optional<uint64_t>>
resolveExport(Program &Program, size_t Module, llvm::StringRef Name,
              std::optional<uint16_t> Ordinal, const ExecutionBudget &Budget,
              ExecutionBackend *CPU = nullptr, ForwardModule Load = {});
llvm::Expected<Environment> prepareEnvironment(AddressSpace &Memory,
                                               const Program &Program,
                                               const ProcessOptions &Options);
llvm::Expected<Program> loadProgram(const std::filesystem::path &Path,
                                    const ProcessOptions &Options,
                                    const ExecutionBudget &Budget,
                                    VirtualMemory &Memory);
} // namespace neverd::emulation::windows_process
#endif

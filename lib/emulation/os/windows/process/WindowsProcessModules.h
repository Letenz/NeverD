//===- WindowsProcessModules.h - Guest startup linking ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_MODULES_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_MODULES_H
#include "WindowsProcess.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <deque>

namespace neverd::emulation::windows_process {
struct ModuleRef {
  size_t Index;
  uint64_t Generation;
  bool operator==(const ModuleRef &) const = default;
};
enum class ModuleState {
  Retired,
  Prepared,
  Linked,
  Initializing,
  Ready,
  Detaching
};
struct Module {
  Image Loaded;
  uint64_t Generation = 0, References = 0;
  ModuleState State = ModuleState::Retired;
  bool Attached = false, Pinned = false;
  bool System = false;
  /// A module outside the catalogue, present only as an identity: every
  /// export query yields an opaque entry.
  bool Opaque = false;
  std::vector<ModuleRef> Dependencies;
  /// Index original entries, retaining holes, aliases and unresolved
  /// forwarders.
  std::map<std::string, size_t> Names;
  std::map<uint32_t, size_t> Ordinals;
  std::vector<PEMetadataRange> ExportMetadata;
};
struct Program {
  std::deque<Module> Modules;
  std::map<std::string, std::filesystem::path> Catalogue;
  std::map<std::string, size_t> Slots;
  uint64_t NextGeneration = 1;
  std::vector<ModuleIdentity> Identities;
  /// Dependency order includes demanded forwarders before attach callbacks.
  std::vector<size_t> AttachOrder;
  /// Loader registration follows ordinary import traversal. Forwarded targets
  /// discovered later do not reorder earlier entries. PEB and detach share it.
  std::vector<size_t> LoaderInitializationOrder;
  /// One exact provider/name gate per process, independent of the caller image.
  std::vector<Import> Gates;
  /// Preparation and subsequent export queries never replenish these credits.
  ImageReadBudget Reads{0, 0};
  /// WindowsProcessOptions::DeferUnmodeled for this process.
  bool DeferUnmodeled = false;
  /// Host path of the input image. ZwOpenFile accepts only this file.
  std::filesystem::path ImagePath;
  /// An executable is its own input. A DLL is loaded by the modeled host's
  /// LoadLibrary call, and becomes observable after its mappings are published.
  std::optional<size_t> InputModule = 0;
  std::string InputName;
  /// Root admission decodes the input once. The first dynamic load consumes
  /// these bytes under the same preparation budget; later reloads reread it.
  std::optional<Image> PreparedInput;
  uint64_t OpaqueEntries = 0;
};
/// A resident identity for the caller's input, independent of the process EXE.
std::optional<size_t> inputModule(const Program &P);
/// A minimal mapped PE host that loads and frees the input through ordinary
/// guest APIs. It never calls an export with a guessed function signature.
llvm::Expected<Image> makeLibraryHost(Program &P, const Image &Input);
inline constexpr uint64_t ModuleCapacity =
    windows_process_limits::Modules + value::SystemModuleCount + 1;
llvm::Error prepareSystemModules(Program &P, VirtualMemory &Memory,
                                 const ExecutionBudget &Budget);
/// The entry for an export identity the model does not implement. One identity
/// has one address for the life of the process; executing it stops the run.
llvm::Expected<uint64_t> opaqueEntry(Program &P, llvm::StringRef Module,
                                     llvm::StringRef Name,
                                     std::optional<uint16_t> Ordinal);
/// Reserve and describe a module that exists only as a loader identity.
llvm::Expected<Image> makeOpaqueImage(Program &P, VirtualMemory &Memory,
                                      llvm::StringRef Name);
/// Guest load failures are distinct from unsupported metadata or transport
/// failures. The running process may handle these errors and continue.
class ModuleLoadError final : public llvm::ErrorInfo<ModuleLoadError> {
public:
  static char ID;
  explicit ModuleLoadError(uint32_t Code) : Code(Code) {}
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
  uint32_t Code;
};
struct ModuleLink {
  size_t Root = 0;
  std::vector<ModuleRef> Added, Attach;
  std::vector<std::pair<ModuleRef, ModuleRef>> Edges;
};
bool resident(const Module &M);
llvm::Error validateImageMetadata(Program &Program, size_t Index,
                                  llvm::ArrayRef<PEMetadataRange> Ranges,
                                  const ExecutionBudget &Budget,
                                  ExecutionBackend &CPU,
                                  llvm::StringRef ChangedDiagnostic);
bool current(const Program &P, ModuleRef Ref);
ModuleRef moduleRef(const Program &P, size_t Index);
std::optional<size_t> findModule(const Program &P, llvm::StringRef Name);
llvm::Expected<ModuleLink> linkModule(Program &P, llvm::StringRef Name,
                                      VirtualMemory &Memory,
                                      const ExecutionBudget &Budget,
                                      ExecutionBackend *CPU = nullptr);
llvm::Error retireModule(Program &P, ModuleRef Ref, VirtualMemory &Memory);
llvm::Expected<std::string> moduleName(llvm::StringRef Name);
using ForwardModule =
    llvm::function_ref<llvm::Expected<size_t>(size_t, llvm::StringRef)>;
struct ExportResolution {
  std::optional<uint64_t> Address;
  /// Guest lookup failure, distinct from an unsupported or malformed chain.
  uint32_t Error;
};
/// Resolve one demanded identity. A missing export is distinct from a malformed
/// chain or an unsupported runtime module load. CPU enables live metadata
/// checks.
llvm::Expected<ExportResolution>
resolveExport(Program &Program, size_t Module, llvm::StringRef Name,
              std::optional<uint16_t> Ordinal, const ExecutionBudget &Budget,
              ExecutionBackend *CPU = nullptr, ForwardModule Load = {});
llvm::Expected<Environment> prepareEnvironment(AddressSpace &Memory,
                                               Program &Program,
                                               const ProcessOptions &Options);
llvm::Error updateEnvironment(AddressSpace &Memory, Program &Program,
                              Environment &Environment,
                              const ExecutionBudget &Budget);
llvm::Error releaseModuleEnvironment(AddressSpace &Memory, Program &Program,
                                     Environment &Environment,
                                     llvm::ArrayRef<ModuleRef> Modules,
                                     const ExecutionBudget &Budget);
llvm::Expected<Program> loadProgram(const std::filesystem::path &Path,
                                    const ProcessOptions &Options,
                                    const ExecutionBudget &Budget,
                                    VirtualMemory &Memory);
} // namespace neverd::emulation::windows_process
#endif

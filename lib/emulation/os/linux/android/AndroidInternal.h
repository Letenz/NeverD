//===- AndroidInternal.h - Android native environment boundaries -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDINTERNAL_H
#define NEVERD_EMULATION_ANDROIDINTERNAL_H
#include "../kernel/LinuxMemory.h"

#include "neverd/emulation/AndroidNative.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include <map>
#include <variant>

namespace neverd::emulation::android_model {
namespace symbol {
#define NEVERD_ANDROID_SYMBOL(Name, Text) inline constexpr char Name[] = Text;
#include "AndroidSymbols.def"
#undef NEVERD_ANDROID_SYMBOL
} // namespace symbol
namespace diagnostic {
#define NEVERD_ANDROID_DIAGNOSTIC(Name, Text)                                  \
  inline constexpr char Name[] = Text;
#include "AndroidDiagnostics.def"
#undef NEVERD_ANDROID_DIAGNOSTIC
} // namespace diagnostic
inline constexpr uint64_t PageSize = 4096;
inline constexpr uint64_t TLSAddress = 0x7000000000;
inline constexpr uint64_t StdioAddress = TLSAddress - PageSize;
inline constexpr uint64_t ThunkBase = TLSAddress + PageSize;
inline constexpr uint64_t ErrnoAddress = TLSAddress + 2 * 8;
inline constexpr uint64_t GuardAddress = TLSAddress + 5 * 8;
inline constexpr uint64_t LinkerErrorAddress = TLSAddress + 0x200;
inline constexpr uint64_t LinkerErrorSlot = TLSAddress + 6 * 8;
inline constexpr uint64_t StackGuard = 0x91ab62e5347c2800;
inline constexpr uint16_t ModelTrap = 0x4e44;
inline constexpr uint64_t ReturnPC = ThunkBase;
inline llvm::Error failure(llvm::Twine Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 diagnostic::FailurePrefix + Message);
}
struct LinkedImage {
  uint64_t Entry, InitialBreak;
  std::vector<uint64_t> Constructors;
  std::map<uint64_t, std::string> Imports;
  std::map<uint64_t, std::string> DynamicProviders;
  std::map<std::string, std::map<std::string, uint64_t>> Libraries;
};
llvm::Expected<LinkedImage> loadImage(AddressSpace &Space,
                                      const BinaryImage &Image,
                                      const ProcessOptions &Options);
/// Populate an already admitted region. File input is bounded by its mapped
/// extent and the workload's preparation deadline, before any CPU is created.
llvm::Error initializeMemoryRegion(AddressSpace &Space,
                                   const NativeMemoryRegion &Region,
                                   const ExecutionBudget &Budget);
struct OnceCallback {
  uint64_t Control;
};
struct FinalizeCallback {
  uint64_t DSO;
};
struct GuestCallback {
  uint64_t Entry;
  std::optional<uint64_t> Argument;
  std::variant<OnceCallback, FinalizeCallback> Continuation;
};
/// A returning model either completes now or suspends for a guest callback.
using BionicValue = std::variant<uint64_t, GuestCallback>;
using BionicResult = llvm::Expected<std::optional<BionicValue>>;
class Bionic {
public:
  Bionic(ExecutionBackend &CPU, linux_model::LinuxMemory &Memory,
         const linux_model::MemoryLayout &Layout, const ProcessOptions &Options,
         ProcessResult &Result, ExecutionBudget &Budget,
         const LinkedImage &Linked)
      : CPU(CPU), Memory(Memory), Layout(Layout), Options(Options),
        Result(Result), Budget(Budget), Linked(Linked) {}
  bool timedOut() const { return Expired; }
  BionicResult invoke(NativeCallEvent &Call);
  BionicResult finishCallback(const GuestCallback &Callback);

private:
  ExecutionBackend &CPU;
  linux_model::LinuxMemory &Memory;
  const linux_model::MemoryLayout &Layout;
  const ProcessOptions &Options;
  ProcessResult &Result;
  ExecutionBudget &Budget;
  const LinkedImage &Linked;
  bool Expired = false;
  struct LibraryState {
    uint64_t Handle = 0, References = 0;
  };
  std::map<std::string, LibraryState> OpenLibraries;
  std::map<uint64_t, std::string> Handles;
  uint64_t NextHandle = 1;
  bool isResident(const std::string &Library) const;
  struct Allocation {
    uint64_t Size, MappedSize;
  };
  std::map<uint64_t, Allocation> Allocations;
  struct ExitCallback {
    uint64_t Entry, Argument, DSO;
  };
  std::vector<ExitCallback> ExitCallbacks;
  llvm::Error access(uint64_t Address, uint64_t Size, unsigned Permissions);
  llvm::Expected<uint8_t> byte(uint64_t Address);
  llvm::Expected<std::string> string(uint64_t Address);
  llvm::Expected<uint64_t> allocate(uint64_t Size);
  llvm::Error release(uint64_t Address);
  llvm::Error setErrno(uint32_t Value);
  llvm::Expected<uint64_t> linkerError(llvm::StringRef Message,
                                       uint64_t ReturnValue = 0);
  BionicResult dlfcn(NativeCallEvent &Call);
  BionicResult once(const NativeCallEvent &Call);
  BionicResult threadAttributes(const NativeCallEvent &Call);
  llvm::Error finishOnce(const OnceCallback &Callback);
  BionicResult registerExit(const NativeCallEvent &Call);
  BionicResult finalize(uint64_t DSO);
  llvm::Expected<uint64_t> tokenize(const NativeCallEvent &Call);
  class StringFormatter;
  BionicResult format(const NativeCallEvent &Call);
  llvm::Expected<std::optional<uint64_t>> mutex(const NativeCallEvent &Call);
};
llvm::Expected<ProcessResult> runNative(const std::filesystem::path &Path,
                                        const ProcessOptions &Options);
} // namespace neverd::emulation::android_model
#endif

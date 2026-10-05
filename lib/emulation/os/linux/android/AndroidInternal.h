//===- AndroidInternal.h - Android environment boundaries ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDINTERNAL_H
#define NEVERD_EMULATION_ANDROIDINTERNAL_H
#include "../kernel/LinuxServices.h"

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
enum class SysconfName : uint32_t {
#define NEVERD_ANDROID_SYSCONF(Name, Value) Name = Value,
#include "AndroidSysconf.def"
#undef NEVERD_ANDROID_SYSCONF
};
namespace thread_attribute_abi {
#define NEVERD_ANDROID_THREAD_ATTRIBUTE_VALUE(Name, Value)                     \
  inline constexpr unsigned Name = Value;
#include "AndroidThreadAttributes.def"
#undef NEVERD_ANDROID_THREAD_ATTRIBUTE_VALUE
} // namespace thread_attribute_abi
namespace once_abi {
#define NEVERD_ANDROID_ONCE_VALUE(Name, Value)                                 \
  inline constexpr unsigned Name = Value;
#include "AndroidOnce.def"
#undef NEVERD_ANDROID_ONCE_VALUE
} // namespace once_abi
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
struct GuestThreadWait {};
struct GuestThreadExit {
  uint64_t Value;
};
/// A returning model either completes now or suspends for a guest callback.
using BionicValue =
    std::variant<uint64_t, GuestCallback, GuestThreadWait, GuestThreadExit>;
using BionicResult = llvm::Expected<std::optional<BionicValue>>;
class GuestThreads;
class Bionic {
public:
  Bionic(ExecutionBackend &CPU, linux_model::LinuxServices &Kernel,
         const linux_model::MemoryLayout &Layout, const ProcessOptions &Options,
         ProcessResult &Result, ExecutionBudget &Budget,
         const LinkedImage &Linked, GuestThreads *Threads = nullptr)
      : CPU(CPU), Kernel(Kernel), Layout(Layout), Options(Options),
        Result(Result), Budget(Budget), Linked(Linked), Threads(Threads) {}
  bool timedOut() const { return Expired; }
  BionicResult invoke(NativeCallEvent &Call);
  BionicResult finishCallback(const GuestCallback &Callback);
  BionicResult resumeMutex(uint64_t Address, uint16_t Attributes);

private:
  ExecutionBackend &CPU;
  linux_model::LinuxServices &Kernel;
  const linux_model::MemoryLayout &Layout;
  const ProcessOptions &Options;
  ProcessResult &Result;
  ExecutionBudget &Budget;
  const LinkedImage &Linked;
  GuestThreads *Threads;
  uint64_t tlsAddress() const;
  uint64_t threadID() const;
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
  BionicResult linkerError(llvm::StringRef Message, uint64_t ReturnValue = 0);
  static std::optional<BionicValue> value(uint64_t Value) {
    return BionicValue(Value);
  }
  BionicResult unsupportedLinking(llvm::StringRef Detail);
  BionicResult kernelCall(std::optional<linux_model::ServiceKind> Kind,
                          const NativeCallEvent &Call);
  BionicResult openLibrary(NativeCallEvent &Call);
  BionicResult lookupSymbol(NativeCallEvent &Call);
  BionicResult closeLibrary(NativeCallEvent &Call);
  BionicResult getLinkerError(NativeCallEvent &Call);
  BionicResult unsupportedImport(const NativeCallEvent &Call);
  BionicResult failCall(const NativeCallEvent &Call);
  BionicResult malloc(const NativeCallEvent &Call);
  BionicResult calloc(const NativeCallEvent &Call);
  BionicResult realloc(const NativeCallEvent &Call);
  BionicResult free(const NativeCallEvent &Call);
  BionicResult copyMemory(const NativeCallEvent &Call);
  BionicResult setMemory(const NativeCallEvent &Call);
  BionicResult compareMemory(const NativeCallEvent &Call);
  BionicResult stringLength(const NativeCallEvent &Call);
  BionicResult compareString(const NativeCallEvent &Call);
  BionicResult getErrno(const NativeCallEvent &Call);
  BionicResult getPageSize(const NativeCallEvent &Call);
  BionicResult getAPILevel(const NativeCallEvent &Call);
  BionicResult queryConfiguration(const NativeCallEvent &Call);
  BionicResult getTime(const NativeCallEvent &Call);
  BionicResult getProperty(const NativeCallEvent &Call);
  BionicResult syscall(const NativeCallEvent &Call);
  BionicResult threadCall(const NativeCallEvent &Call);
  BionicResult finalizeCall(const NativeCallEvent &Call);
  BionicResult once(const NativeCallEvent &Call);
  class ThreadAttributes;
  BionicResult threadAttributes(const NativeCallEvent &Call);
  llvm::Error finishOnce(const OnceCallback &Callback);
  BionicResult registerExit(const NativeCallEvent &Call);
  BionicResult finalize(uint64_t DSO);
  BionicResult tokenize(const NativeCallEvent &Call);
  BionicResult findCharacter(const NativeCallEvent &Call);
  class ArgumentReader;
  class StringFormatter;
  BionicResult format(const NativeCallEvent &Call);
  class IntegerScanner;
  BionicResult scan(const NativeCallEvent &Call);
  class Mutex;
  BionicResult mutex(const NativeCallEvent &Call);
  BionicResult resetMutexAttribute(const NativeCallEvent &Call, uint64_t Value);
  BionicResult initializeMutexAttribute(const NativeCallEvent &Call);
  BionicResult destroyMutexAttribute(const NativeCallEvent &Call);
  BionicResult mutexAttributes(const NativeCallEvent &Call);
  BionicResult initializeMutex(const NativeCallEvent &Call);
  std::optional<BionicValue> unsupportedMutex(llvm::StringRef Name,
                                              llvm::StringRef Reason);
  llvm::Error mutexAccess(uint64_t Address, unsigned Size, unsigned Permissions,
                          unsigned Alignment);
  llvm::Expected<uint64_t> readMutexWord(uint64_t Address, unsigned Size,
                                         unsigned Alignment);
  llvm::Error writeMutexWord(uint64_t Address, unsigned Size, uint64_t Value);
};
llvm::Expected<ProcessResult> runNative(const std::filesystem::path &Path,
                                        const ProcessOptions &Options);
} // namespace neverd::emulation::android_model
#endif

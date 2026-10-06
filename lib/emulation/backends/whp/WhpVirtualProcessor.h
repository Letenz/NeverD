//===- WhpVirtualProcessor.h - Shared partition and isolated VP ownership
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_VIRTUALPROCESSOR_H
#define NEVERD_EMULATION_WHP_VIRTUALPROCESSOR_H
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../RunDeadline.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

#include <bitset>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <windows.h>
#if __has_include(<WinHvPlatform.h>)
#include <WinHvPlatform.h>
#else
#include <winhvplatform.h>
#endif
namespace neverd::emulation {
#define NEVERD_WHP_STRING(Name, Text) constexpr auto Name = Text;
#define NEVERD_WHP_TEXT(Name, Text) constexpr auto Name = Text;
#define NEVERD_WHP_VALUE(Name, Value) inline constexpr uint32_t Name = Value;
#define NEVERD_WHP_ADDRESS(Name, Value) inline constexpr uint64_t Name = Value;
#include "WhpProtocol.def"
#undef NEVERD_WHP_ADDRESS
#undef NEVERD_WHP_VALUE
#undef NEVERD_WHP_TEXT
#undef NEVERD_WHP_STRING
namespace whp::operation {
#define NEVERD_WHP_FUNCTION(Name) inline constexpr char Name[] = #Name;
#define NEVERD_WHP_X64_OPTIONAL_FUNCTION(Name) NEVERD_WHP_FUNCTION(Name)
#include "WhpProtocol.def"
#undef NEVERD_WHP_X64_OPTIONAL_FUNCTION
#undef NEVERD_WHP_FUNCTION
} // namespace whp::operation
inline std::string whpFailure(const char *Text, HRESULT Status,
                              const char *Operation = nullptr) {
  return Operation ? llvm::formatv(OperationFailure, Text, uint32_t(Status),
                                   Operation)
                         .str()
                   : llvm::formatv(HostFailure, Text, uint32_t(Status)).str();
}
inline llvm::Error whpError(const char *Text, HRESULT Status,
                            const char *Operation = nullptr) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 whpFailure(Text, Status, Operation));
}
inline llvm::Error whpUnavailable(const char *Text, HRESULT Status,
                                  const char *Operation) {
  return llvm::make_error<BackendUnavailableError>(
      whpFailure(Text, Status, Operation),
      BackendAvailability::MissingCapability);
}
struct WhpAPI {
  HMODULE Module = nullptr;
#define NEVERD_WHP_FUNCTION(Name) decltype(&::Name) Name = nullptr;
#define NEVERD_WHP_X64_OPTIONAL_FUNCTION(Name) NEVERD_WHP_FUNCTION(Name)
#include "WhpProtocol.def"
#undef NEVERD_WHP_X64_OPTIONAL_FUNCTION
#undef NEVERD_WHP_FUNCTION
  ~WhpAPI() {
    if (Module)
      FreeLibrary(Module);
  }
  llvm::Error load() {
    Module = LoadLibraryExW(Library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!Module)
      return diagnostic::unavailable(diagnostic::WhpCapability,
                                     BackendAvailability::HostAPI);
#define NEVERD_WHP_FUNCTION(Name)                                              \
  Name = reinterpret_cast<decltype(Name)>(GetProcAddress(Module, #Name));      \
  if (!Name)                                                                   \
    return diagnostic::unavailable(diagnostic::WhpCapability,                  \
                                   BackendAvailability::HostAPI);
#include "WhpProtocol.def"
#undef NEVERD_WHP_FUNCTION
#define NEVERD_WHP_X64_OPTIONAL_FUNCTION(Name)                                 \
  Name = reinterpret_cast<decltype(Name)>(GetProcAddress(Module, #Name));
#include "WhpProtocol.def"
#undef NEVERD_WHP_X64_OPTIONAL_FUNCTION
    return llvm::Error::success();
  }
};
struct WhpInterrupt {
  WhpAPI &API;
  WHV_PARTITION_HANDLE Partition;
  uint32_t ProcessorIndex;

  void operator()() const noexcept {
    API.WHvCancelRunVirtualProcessor(Partition, ProcessorIndex, 0);
  }
};
/// Windows may reject a second mapped partition in one process. All logical
/// CPUs therefore share one native partition, with separate VP state and GPA
/// windows. The registry lock serializes initial creation against final close.
struct WhpPartitionHost {
  using Configure =
      llvm::function_ref<llvm::Error(WhpAPI &, WHV_PARTITION_HANDLE &)>;
  WhpAPI API;
  WHV_PARTITION_HANDLE Partition = nullptr;
  std::bitset<ProcessorCount> Processors;
  ~WhpPartitionHost() {
    if (Partition && FAILED(API.WHvDeletePartition(Partition)))
      llvm::report_fatal_error(diagnostic::WhpRetire);
  }
  static llvm::Expected<std::shared_ptr<WhpPartitionHost>>
  acquire(Configure Setup) {
    auto &Host = cached();
    if (!Host) {
      auto Next = std::make_shared<WhpPartitionHost>();
      if (auto E = Next->API.load())
        return E;
      if (auto E = Setup(Next->API, Next->Partition))
        return E;
      Host = std::move(Next);
    }
    return Host;
  }
  /// The caller holds this lock through slot acquisition or final retirement.
  /// No detached shared pointer can keep an idle old partition alive while a
  /// replacement is being created in another thread.
  static std::mutex &mutex() {
    static std::mutex Mutex;
    return Mutex;
  }
  static void retireIdle(std::shared_ptr<WhpPartitionHost> &Owner) {
    if (Owner == cached() && Owner->Processors.none())
      cached().reset();
    Owner.reset();
  }

private:
  static std::shared_ptr<WhpPartitionHost> &cached() {
    static std::shared_ptr<WhpPartitionHost> Host;
    return Host;
  }
};
/// A VP lease within the common host partition. Injected API protocol fixtures
/// may instead own their entire test handle.
class WhpVirtualProcessor {
public:
  WhpAPI API;
  WHV_PARTITION_HANDLE Partition = nullptr;
  uint32_t ProcessorIndex = 0;
  virtual ~WhpVirtualProcessor() {
    // The interrupt worker must stop before its partition and API are retired.
    Watchdog.reset();
    if (Host) {
      std::lock_guard Lock(WhpPartitionHost::mutex());
      if (ProcessorCreated &&
          FAILED(API.WHvDeleteVirtualProcessor(Partition, ProcessorIndex)))
        llvm::report_fatal_error(diagnostic::WhpRetire);
      for (const auto &Mapping : llvm::ArrayRef(Mapped).take_front(MappedCount))
        if (FAILED(API.WHvUnmapGpaRange(Partition, Mapping.Physical,
                                        Mapping.Size)))
          llvm::report_fatal_error(diagnostic::WhpRetire);
      Host->Processors.reset(ProcessorIndex);
      WhpPartitionHost::retireIdle(Host);
    } else if (Partition)
      API.WHvDeletePartition(Partition);
  }
  llvm::Error attach(MemoryProjection &Memory,
                     WhpPartitionHost::Configure Setup) {
    if (auto E = API.load())
      return E;
    std::lock_guard Lock(WhpPartitionHost::mutex());
    auto Shared = WhpPartitionHost::acquire(Setup);
    if (!Shared)
      return Shared.takeError();
    auto Release =
        llvm::scope_exit([&] { WhpPartitionHost::retireIdle(*Shared); });
    unsigned Index = Memory.parallelEnabled() ? 1 : 0;
    if (Memory.parallelEnabled())
      while (Index < ProcessorCount && (*Shared)->Processors.test(Index))
        ++Index;
    if (Index == ProcessorCount || (*Shared)->Processors.test(Index))
      return diagnostic::error(diagnostic::WhpProcessors);
    const uint64_t Base =
        Index ? ParallelWindowBase + (Index - 1) * ProcessorWindowSize : 0;
    if (Index) {
      WHV_CAPABILITY Capability{};
      const auto Status =
          API.WHvGetCapability(WHvCapabilityCodePhysicalAddressWidth,
                               &Capability, sizeof(Capability), nullptr);
      if (FAILED(Status))
        return whpUnavailable(diagnostic::WhpAddressWidth, Status,
                              whp::operation::WHvGetCapability);
      const auto Bits = Capability.PhysicalAddressWidth;
      constexpr auto WordBits = std::numeric_limits<uint64_t>::digits;
      if (!Bits || Bits > WordBits ||
          (Bits < WordBits && ((Base + ProcessorWindowSize - 1) >> Bits)))
        return diagnostic::unavailable(diagnostic::WhpAddressWidth,
                                       BackendAvailability::MissingCapability);
    }
    Host = *Shared;
    Host->Processors.set(Index);
    ProcessorIndex = Index;
    Partition = Host->Partition;
    if (Index)
      if (auto E = Memory.relocateTransport(Base))
        return E;
    if (auto E = mapMemory(Memory))
      return E;
    if (const auto Status =
            API.WHvCreateVirtualProcessor(Partition, ProcessorIndex, 0);
        FAILED(Status))
      return whpError(diagnostic::WhpCreate, Status,
                      whp::operation::WHvCreateVirtualProcessor);
    ProcessorCreated = true;
    return llvm::Error::success();
  }
  llvm::Error mapMemory(const MemoryProjection &Memory) {
    for (const auto &Mapping : Memory.registrations()) {
      const auto Status = API.WHvMapGpaRange(
          Partition, Mapping.Backing, Mapping.Physical, Mapping.Size,
          WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
              WHvMapGpaRangeFlagExecute);
      if (FAILED(Status))
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            llvm::formatv(MapFailure, diagnostic::WhpMap, uint32_t(Status),
                          Mapping.Physical, Mapping.Size)
                .str());
      Mapped[MappedCount++] = Mapping;
    }
    return llvm::Error::success();
  }
  llvm::Error initializeRunControl() {
    try {
      Watchdog = std::make_unique<RunDeadline<WhpInterrupt>>(
          WhpInterrupt{API, Partition, ProcessorIndex});
    } catch (const std::system_error &) {
      return diagnostic::error(diagnostic::WhpRunControl);
    }
    return llvm::Error::success();
  }
  /// Complete validates a successfully returned native exit on this caller,
  /// before classifying cancellation. It must stage ordinary state privately;
  /// an authenticated ISA exception or real capture failure retains priority.
  llvm::Error run(WHV_RUN_VP_EXIT_CONTEXT &Exit, MachineRunControl Control,
                  llvm::function_ref<llvm::Error()> Complete = {}) {
    if (!Watchdog)
      return diagnostic::error(diagnostic::WhpRunControl);
    // step() creates one allowance for preparation, entry and state capture.
    // Starting another allowance here would extend the instruction deadline.
    const auto Entry = Watchdog->invoke(Control, [&]() noexcept {
      return API.WHvRunVirtualProcessor(Partition, ProcessorIndex, &Exit,
                                        sizeof(Exit));
    });
    if (Entry.Value && FAILED(*Entry.Value))
      return whpError(diagnostic::WhpRun, *Entry.Value,
                      whp::operation::WHvRunVirtualProcessor);
    if (!Entry.Value ||
        (Entry.Cancelled && Exit.ExitReason == WHvRunVpExitReasonCanceled))
      return diagnostic::interrupted(diagnostic::WhpRun, Control);
    // A successful host return may describe a processor exception. Authenticate
    // that result before allowing a concurrent stop to hide it. The watchdog
    // has already retired its borrow; capture retains the same step deadline.
    if (Complete)
      if (auto E = Complete())
        return E;
    if (Entry.Cancelled || Control.interrupted())
      return diagnostic::interrupted(diagnostic::WhpRun, Control);
    return llvm::Error::success();
  }

private:
  std::shared_ptr<WhpPartitionHost> Host;
  std::array<MemoryRegistration, 2> Mapped{};
  unsigned MappedCount = 0;
  bool ProcessorCreated = false;
  std::unique_ptr<RunDeadline<WhpInterrupt>> Watchdog;
};
} // namespace neverd::emulation
#endif

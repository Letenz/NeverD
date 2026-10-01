//===- WhpPartition.h - Dynamically loaded WHP resource ownership ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_PARTITION_H
#define NEVERD_EMULATION_WHP_PARTITION_H
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/MemoryProjection.h"
#include "../RunDeadline.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <memory>
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
#include "WhpProtocol.def"
#undef NEVERD_WHP_VALUE
#undef NEVERD_WHP_TEXT
#undef NEVERD_WHP_STRING
inline llvm::Error whpError(const char *Text, HRESULT Status) {
  return llvm::createStringError(
      llvm::inconvertibleErrorCode(),
      llvm::formatv(HostFailure, Text, uint32_t(Status)).str());
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

  void operator()() const noexcept {
    API.WHvCancelRunVirtualProcessor(Partition, 0, 0);
  }
};
class WhpPartition {
public:
  WhpAPI API;
  WHV_PARTITION_HANDLE Partition = nullptr;
  virtual ~WhpPartition() {
    // The interrupt worker must stop before its partition and API are retired.
    Watchdog.reset();
    if (Partition)
      API.WHvDeletePartition(Partition);
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
    }
    return llvm::Error::success();
  }
  llvm::Error initializeRunControl() {
    try {
      Watchdog = std::make_unique<RunDeadline<WhpInterrupt>>(
          WhpInterrupt{API, Partition});
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
      return API.WHvRunVirtualProcessor(Partition, 0, &Exit, sizeof(Exit));
    });
    if (Entry.Value && FAILED(*Entry.Value))
      return diagnostic::error(diagnostic::WhpRun);
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
  std::unique_ptr<RunDeadline<WhpInterrupt>> Watchdog;
};
} // namespace neverd::emulation
#endif

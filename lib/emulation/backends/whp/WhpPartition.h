//===- WhpPartition.h - Dynamically loaded WHP resource ownership ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_PARTITION_H
#define NEVERD_EMULATION_WHP_PARTITION_H
#include "../../core/ExecutionDiagnostics.h"
#include "../RunDeadline.h"

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
#include "WhpProtocol.def"
#undef NEVERD_WHP_STRING
struct WhpAPI {
  HMODULE Module = nullptr;
#define NEVERD_WHP_FUNCTION(Name) decltype(&::Name) Name = nullptr;
#include "WhpProtocol.def"
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
  llvm::Error initializeRunControl() {
    try {
      Watchdog = std::make_unique<RunDeadline<WhpInterrupt>>(
          WhpInterrupt{API, Partition});
    } catch (const std::system_error &) {
      return diagnostic::error(diagnostic::WhpRunControl);
    }
    return llvm::Error::success();
  }
  llvm::Error run(WHV_RUN_VP_EXIT_CONTEXT &Exit, MachineRunControl Control) {
    Watchdog->arm(Control.forNativeStep());
    const HRESULT Result =
        API.WHvRunVirtualProcessor(Partition, 0, &Exit, sizeof(Exit));
    // Cancellation may race with a completed instruction. Never publish a
    // successful register transfer when native progress is uncertain.
    if (Watchdog->disarm() || FAILED(Result))
      return diagnostic::error(diagnostic::WhpRun);
    return llvm::Error::success();
  }

private:
  std::unique_ptr<RunDeadline<WhpInterrupt>> Watchdog;
};
} // namespace neverd::emulation
#endif

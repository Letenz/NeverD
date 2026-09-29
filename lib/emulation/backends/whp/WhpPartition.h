//===- WhpPartition.h - Dynamically loaded WHP resource ownership ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WHP_PARTITION_H
#define NEVERD_EMULATION_WHP_PARTITION_H
#include "../../core/ExecutionDiagnostics.h"

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
class WhpPartition {
public:
  WhpAPI API;
  WHV_PARTITION_HANDLE Partition = nullptr;
  virtual ~WhpPartition() {
    if (Partition)
      API.WHvDeletePartition(Partition);
  }
};
} // namespace neverd::emulation
#endif

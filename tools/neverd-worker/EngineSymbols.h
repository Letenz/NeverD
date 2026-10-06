#pragma once

#include "neverd/sdk/NeverDCAPISession.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace neverd::worker {

/// Resolve an additive C ABI entry point from the already linked engine, never
/// from an arbitrary user path.  An older matching engine lacks newer symbols;
/// callers keep their documented fallback.
template <typename FunctionT> FunctionT engineSymbol(const char *name) {
#ifdef _WIN32
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&neverd_session_create),
                          &module))
    return nullptr;
  return reinterpret_cast<FunctionT>(GetProcAddress(module, name));
#else
  return reinterpret_cast<FunctionT>(dlsym(RTLD_DEFAULT, name));
#endif
}

} // namespace neverd::worker

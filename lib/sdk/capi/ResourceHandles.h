//===- ResourceHandles.h - Owned C ABI resource identities -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned C ABI resource identities.
///

#ifndef NEVERD_SDK_RESOURCEHANDLES_H
#define NEVERD_SDK_RESOURCEHANDLES_H
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <exception>
#include <memory>

namespace neverd::sdk::resources {
enum class HandleKind { ModuleSet, Execution };
llvm::Expected<uint64_t> retain(HandleKind, std::shared_ptr<void>);
llvm::Expected<std::shared_ptr<void>> acquire(uint64_t, HandleKind);
llvm::Error release(uint64_t, HandleKind);
llvm::Expected<llvm::StringRef> text(const char *, uint64_t Limit,
                                     llvm::StringRef What,
                                     bool AllowEmpty = false);
llvm::Expected<const char *>
ownedString(llvm::StringRef, uint64_t Limit = kMaxOwnedResourceStringBytes);
void reportError(const char **, llvm::StringRef) noexcept;

/// No exception crosses a C boundary, including allocation failures while
/// producing diagnostics. Errors/results use the existing free_string
/// allocator.
template <typename T, typename F>
T boundary(const char **Error, T Failure, F &&Body) noexcept {
  if (Error)
    *Error = nullptr;
  try {
    llvm::Expected<T> Result = Body();
    if (!Result) {
      reportError(Error, llvm::toString(Result.takeError()));
      return Failure;
    }
    return std::move(*Result);
  } catch (const std::exception &E) {
    reportError(Error, E.what());
  } catch (...) {
    reportError(Error,
                "resource operation raised an unexpected host exception");
  }
  return Failure;
}
} // namespace neverd::sdk::resources
#endif

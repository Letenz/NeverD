//===- KernelMDLCalls.cpp - MDL mapping calls -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::mapMDL(llvm::ArrayRef<uint64_t> A) {
  auto Cache = memoryCacheType(static_cast<uint32_t>(A[2]));
  if (!Cache)
    return Cache.takeError();
  if (static_cast<uint8_t>(A[1]) == UserMode)
    return mapUserMDL(A[0], A[3], static_cast<uint32_t>(A[5]), *Cache);
  if (static_cast<uint8_t>(A[1]) != KernelMode || A[3])
    return modelError("MDL mapping requires KernelMode and no requested "
                      "address");
  auto Mapped =
      mapLockedPages(A[0], static_cast<uint32_t>(A[5]), false, *Cache);
  if (!Mapped)
    return Mapped.takeError();
  // The legacy wrapper selects cached pages, HIGH priority and bugcheck on
  // shortage. Successful mappings share the same owner; a shortage must not
  // become a successful NULL return when the caller requested a kernel stop.
  if (!*Mapped && static_cast<uint8_t>(A[4]))
    return modelError("kernel MDL mapping shortage requires an unmodeled "
                      "bugcheck");
  return *Mapped;
}

llvm::Expected<uint64_t> KernelModel::unmapMDL(llvm::ArrayRef<uint64_t> A) {
  if (A[0] >= profile::UserMappedAliasBase &&
      A[0] - profile::UserMappedAliasBase < profile::UserMappedAliasSize) {
    if (auto E = unmapUserMDL(A[0], A[1]))
      return E;
    return 0;
  }
  if (auto E = unmapLockedPages(A[0], A[1]))
    return E;
  return 0;
}

} // namespace neverd::emulation

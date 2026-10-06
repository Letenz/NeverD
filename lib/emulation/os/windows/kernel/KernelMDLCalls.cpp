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
  if (static_cast<uint8_t>(A[1]) != KernelMode || A[3] ||
      static_cast<uint8_t>(A[4]))
    return modelError("MDL mapping requires KernelMode, no requested "
                      "address and no bugcheck");
  return mapLockedPages(A[0], static_cast<uint32_t>(A[5]), false, *Cache);
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

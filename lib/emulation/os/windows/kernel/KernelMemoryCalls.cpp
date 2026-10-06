//===- KernelMemoryCalls.cpp - Checked guest memory -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelAPIKind.h"
#include "KernelException.h"
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

namespace runtime {
llvm::Error checkBufferRange(uint64_t Address, uint64_t Size) {
  if (Size && (!Address || Size - 1 > UINT64_MAX - Address))
    return modelError("invalid or overflowing guest buffer");
  if (Size > profile::KernelArenaSize)
    return modelError("API buffer exceeds the 1 MiB model operation limit");
  return llvm::Error::success();
}
llvm::Error writeBytes(GuestMemory &Memory, uint64_t Address, uint64_t Size,
                       uint8_t Value) {
  if (auto E = checkBufferRange(Address, Size))
    return E;
  if (!Size)
    return llvm::Error::success();
  return Memory.write(Address, std::vector<uint8_t>(Size, Value));
}
} // namespace runtime

llvm::Expected<uint64_t> KernelModel::memoryCall(KernelAPIKind Kind,
                                                 llvm::ArrayRef<uint64_t> A) {
  const bool Zero = Kind == KernelAPIKind::RtlZeroMemory;
  const bool Fill = Kind == KernelAPIKind::RtlFillMemory;
  const uint64_t Size = Zero || Fill ? A[1] : A[2];
  auto PreflightUser = [&](uint64_t Address, bool IsWrite) -> llvm::Error {
    if (!Size || Address >= profile::UserProbeLimit)
      return llvm::Error::success();
    auto Allowed = Memory.canAccess(Address, Size, IsWrite ? Write : Read);
    if (!Allowed)
      return Allowed.takeError();
    if (!*Allowed)
      return llvm::make_error<KernelGuestException>(
          exceptions::StatusAccessViolation);
    return llvm::Error::success();
  };
  if (auto E = runtime::checkBufferRange(A[0], Size))
    return E;
  if (Zero || Fill || Kind == KernelAPIKind::memset) {
    const uint8_t Value = Zero ? 0 : static_cast<uint8_t>(Fill ? A[2] : A[1]);
    if (auto E = validateGuestAccess(A[0], Size, true))
      return E;
    if (auto E = PreflightUser(A[0], true))
      return E;
    if (auto E = runtime::writeBytes(Memory, A[0], Size, Value))
      return E;
    return Kind == KernelAPIKind::memset ? A[0] : 0;
  }
  if (auto E = runtime::checkBufferRange(A[1], Size))
    return E;
  if ((Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::RtlCopyMemory) &&
      Size && (A[0] < A[1] ? A[1] - A[0] < Size : A[0] - A[1] < Size))
    return modelError(
        "overlapping buffers passed to a non-overlapping copy API");
  std::vector<uint8_t> Source(Size);
  if (auto E = validateGuestAccess(A[1], Size, false))
    return E;
  if (auto E = PreflightUser(A[1], false))
    return E;
  if (Size)
    if (auto E = Memory.read(A[1], Source))
      return E;
  if (Kind == KernelAPIKind::memcmp ||
      Kind == KernelAPIKind::RtlCompareMemory) {
    std::vector<uint8_t> Other(Size);
    if (auto E = validateGuestAccess(A[0], Size, false))
      return E;
    if (auto E = PreflightUser(A[0], false))
      return E;
    if (Size)
      if (auto E = Memory.read(A[0], Other))
        return E;
    for (size_t I = 0; I < Size; ++I)
      if (Other[I] != Source[I])
        return Kind == KernelAPIKind::RtlCompareMemory
                   ? uint64_t(I)
                   : uint64_t(static_cast<uint32_t>(int(Other[I]) - Source[I]));
    return Kind == KernelAPIKind::RtlCompareMemory ? Size : 0;
  }
  if (auto E = validateGuestAccess(A[0], Size, true))
    return E;
  if (auto E = PreflightUser(A[0], true))
    return E;
  if (Size)
    if (auto E = Memory.write(A[0], Source))
      return E;
  return Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::memmove ? A[0]
                                                                         : 0;
}

} // namespace neverd::emulation

//===- KernelFrameworkLocks.cpp - Framework lock object lifetimes ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Framework handles retain typed contexts and normal object ownership;
/// executive/dispatcher models own the actual spin and wait lock state.
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "KernelScheduler.h"
#include "WindowsKernelLayout.h"

#include <bit>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error lockError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "framework lock: " + Message);
}
} // namespace

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callLock(llvm::StringRef Name, Binding &B,
                          llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  const bool Create =
      Name == api::WdfSpinLockCreate || Name == api::WdfWaitLockCreate;
  const bool Acquire =
      Name == api::WdfSpinLockAcquire || Name == api::WdfWaitLockAcquire;
  const bool Release =
      Name == api::WdfSpinLockRelease || Name == api::WdfWaitLockRelease;
  if (!Create && !Acquire && !Release)
    return std::optional<uint64_t>{};
  const bool Wait = Name == api::WdfWaitLockCreate ||
                    Name == api::WdfWaitLockAcquire ||
                    Name == api::WdfWaitLockRelease;
  if (IRQL > scheduler::DispatchLevel)
    return lockError("lock operation requires IRQL <= DISPATCH_LEVEL");
  if (Create) {
    auto Validation = attributes(A[1], AttributesUse::Object);
    if (!Validation)
      return Validation.takeError();
    if (const auto *Status = std::get_if<uint32_t>(&*Validation))
      return std::optional<uint64_t>{*Status};
    const auto &Attrs = std::get<Attributes>(*Validation);
    if (!LocksHost.Create || !LocksHost.Destroy)
      return lockError("lock creation host is unavailable");
    if (auto E = writable(A[2], sizeof(uint64_t)))
      return E;
    auto Storage = LocksHost.Create(Wait);
    if (!Storage)
      return Storage.takeError();
    auto Handle = createObject(B.Globals, Attrs, false);
    if (!Handle)
      return llvm::joinErrors(Handle.takeError(),
                              LocksHost.Destroy(*Storage, Wait));
    Objects.at(*Handle).Kind =
        Wait ? ObjectKind::WaitLock : ObjectKind::SpinLock;
    LockObjects.emplace(*Handle, Lock{*Storage, Wait, {}});
    if (auto E = Memory.writeInteger(A[2], *Handle, sizeof(uint64_t)))
      return E;
    return std::optional<uint64_t>{windows::StatusSuccess};
  }
  auto Object = Objects.find(A[1]);
  auto Lock = LockObjects.find(A[1]);
  if (Object == Objects.end() || Object->second.Binding != B.Globals ||
      Object->second.Deleting || Lock == LockObjects.end() ||
      Lock->second.Wait != Wait)
    return lockError("operation requires a live matching framework lock");
  if (!Wait && !Lock->second.InterruptUsers.empty())
    return lockError("an interrupt spin lock requires WdfInterrupt lock APIs");
  if (Release) {
    if (!LocksHost.Release)
      return lockError("lock release host is unavailable");
    if (auto E = LocksHost.Release(Lock->second.Storage, Wait))
      return E;
    return std::optional<uint64_t>{0};
  }
  std::optional<int64_t> Timeout;
  if (Wait && A[2]) {
    auto Value = read(A[2], sizeof(int64_t));
    if (!Value)
      return Value.takeError();
    Timeout = std::bit_cast<int64_t>(*Value);
  }
  // The Microsoft DDI page explicitly restricts zero-timeout polling to IRQL
  // below DISPATCH_LEVEL, despite the broader SAL in wdfsync.h.
  if (Wait && (Timeout == 0 ? IRQL >= scheduler::DispatchLevel : IRQL != 0))
    return lockError("wait lock requires PASSIVE_LEVEL, or IRQL below "
                     "DISPATCH_LEVEL for a zero timeout");
  if (!LocksHost.Acquire)
    return lockError("lock acquisition host is unavailable");
  auto Status = LocksHost.Acquire(Lock->second.Storage, Wait, Timeout);
  if (!Status)
    return Status.takeError();
  return std::optional<uint64_t>{*Status};
}

llvm::Error KernelFramework::validateLockDeletion(uint64_t Handle,
                                                  uint64_t Root) const {
  auto Lock = LockObjects.find(Handle);
  if (Lock == LockObjects.end() || !LocksHost.CanDelete)
    return lockError("deletion lost its lock authority");
  if (auto E = LocksHost.CanDelete(Lock->second.Storage, Lock->second.Wait))
    return E;
  if (Handle != Root)
    for (uint64_t User : Lock->second.InterruptUsers) {
      auto Object = Objects.find(User);
      while (Object != Objects.end() && Object->first != Root)
        Object = Objects.find(Object->second.Parent);
      if (Object == Objects.end())
        return lockError(
            "parent deletion retains an external interrupt's lock");
    }
  return llvm::Error::success();
}

llvm::Error KernelFramework::destroyFrameworkLock(uint64_t Handle) {
  auto Lock = LockObjects.find(Handle);
  if (Lock == LockObjects.end() || !LocksHost.Destroy ||
      !Lock->second.InterruptUsers.empty())
    return lockError("destruction lost its unreferenced lock authority");
  if (auto E = LocksHost.Destroy(Lock->second.Storage, Lock->second.Wait))
    return E;
  LockObjects.erase(Lock);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::releaseInterruptLock(uint64_t Handle) {
  const uint64_t LockHandle = InterruptObjects.at(Handle).ExternalLock;
  if (!LockHandle)
    return std::optional<uint64_t>{};
  auto Lock = LockObjects.find(LockHandle);
  auto Object = Objects.find(LockHandle);
  if (Lock == LockObjects.end() || Object == Objects.end() ||
      !Object->second.InternalReferences ||
      !Lock->second.InterruptUsers.erase(Handle))
    return lockError("interrupt destruction lost its retained external lock");
  --Object->second.InternalReferences;
  const auto &O = Object->second;
  if (O.Cleaned && O.DestroyEligible && !O.References && !O.InternalReferences)
    return std::optional<uint64_t>{LockHandle};
  return std::optional<uint64_t>{};
}
} // namespace neverd::emulation

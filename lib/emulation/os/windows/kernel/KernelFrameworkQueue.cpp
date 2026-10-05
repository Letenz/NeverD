//===- KernelFrameworkQueue.cpp - KMDF control queue state ----------------=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original model of KMDF queue creation and device association. Public ABI:
/// Microsoft's wdfio.h 1.33; validation and ownership follow fxioqueueapi.cpp,
/// fxioqueue.cpp and fxpkgio.cpp at b6191d9543441329154da32f7ab9bdd97228dd3c.
/// Configuration never invokes callbacks or completes an underlying WDM IRP.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error invalidQueue(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF queue: " + Message);
}
} // namespace

bool KernelFramework::queuePnpHeld(const Queue &Q) const {
  return Q.PowerManaged && Devices.at(Q.Device).queuesHeld();
}

llvm::Expected<uint64_t>
KernelFramework::callQueueCreate(llvm::StringRef, Binding &B,
                                 llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueDeviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Device = **Selected;
  if (Objects.at(A[1]).Deleting)
    return invalidQueue("cannot create a queue while its device is deleting");

  auto Validation = attributes(A[3], AttributesUse::Object);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  auto Attrs = std::get<Attributes>(*Validation);

  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size == QueueConfigV17Size || *Size == QueueConfigV19Size)
    return invalidQueue("legacy queue configuration is not modeled");
  if (*Size != QueueConfigSize)
    return InfoLengthMismatch;
  if (auto E = ValidateAccess(A[2], QueueConfigSize, false))
    return E;
  std::vector<uint8_t> Config(QueueConfigSize);
  if (auto E = Memory.read(A[2], Config))
    return E;
  const auto Read32 = [&](uint64_t Offset) {
    return llvm::support::endian::read32le(Config.data() + Offset);
  };
  const auto Read64 = [&](uint64_t Offset) {
    return llvm::support::endian::read64le(Config.data() + Offset);
  };
  const auto Dispatch = Read32(QueueConfigDispatch);
  const bool IsDefault = Config[QueueConfigIsDefault] != 0;
  if (!IsDefault && !A[4])
    return QueueInvalidOutputParameter;
  if (IsDefault && Device.Initialized)
    return QueueInvalidDeviceState;

  if (Attrs.Parent) {
    uint64_t Ancestor = Attrs.Parent;
    for (size_t Remaining = Objects.size(); Ancestor; --Remaining) {
      auto Parent = Objects.find(Ancestor);
      if (!Remaining || Parent == Objects.end() ||
          Parent->second.Binding != B.Globals || Parent->second.Deleting)
        return invalidQueue("invalid or deleting explicit queue parent");
      if (Parent->second.Kind == ObjectKind::Device)
        break;
      Ancestor = Parent->second.Parent;
    }
    if (Ancestor != A[1])
      return QueueInvalidDeviceRequest;
    if (Attrs.Parent != A[1])
      return invalidQueue(
          "queue parenting below a device child is not modeled");
  }

  const uint64_t ConfigDriver = Read64(QueueConfigDriver);
  if (ConfigDriver && ConfigDriver != B.DriverHandle)
    return invalidQueue("queue driver is not this binding's live driver");
  if (Dispatch < QueueDispatchSequential || Dispatch > QueueDispatchManual)
    return InvalidParameter;
  const uint64_t Default = Read64(QueueConfigDefault);
  const uint64_t Read = Read64(QueueConfigRead);
  const uint64_t Write = Read64(QueueConfigWrite);
  const uint64_t DeviceControl = Read64(QueueConfigDeviceControl);
  const uint64_t Internal = Read64(QueueConfigInternalDeviceControl);
  const bool HasCallback =
      Default || Read || Write || DeviceControl || Internal;
  if (Dispatch != QueueDispatchManual && !HasCallback)
    return QueueNoCallback;
  if ((Dispatch == QueueDispatchManual && HasCallback) ||
      (Dispatch != QueueDispatchParallel &&
       Read32(QueueConfigPresentedRequests)))
    return InvalidParameter;
  const uint32_t PowerManaged = Read32(QueueConfigPowerManaged);
  if (PowerManaged > QueuePowerUseDefault)
    return invalidQueue("invalid power-management tri-state");
  if (Dispatch == QueueDispatchParallel &&
      !Read32(QueueConfigPresentedRequests))
    return InvalidParameter;
  if (Internal)
    return invalidQueue("internal-device-control callbacks are not modeled");
  const uint64_t IoStop = Read64(QueueConfigStop);
  const uint64_t IoResume = Read64(QueueConfigResume);
  const bool EffectivePowerManagement =
      Device.PDO && (PowerManaged == QueuePowerEnabled ||
                     PowerManaged == QueuePowerUseDefault);
  if ((IoStop || IoResume) && !EffectivePowerManagement)
    return invalidQueue(
        "I/O stop and resume require a power-managed PnP queue");

  const auto &ParentObject = Objects.at(A[1]);
  const uint32_t Execution = Attrs.Execution == ExecutionInherit
                                 ? ParentObject.Execution
                                 : Attrs.Execution;
  const uint32_t Synchronization =
      Attrs.Synchronization == SynchronizationInherit
          ? ParentObject.Synchronization
          : Attrs.Synchronization;
  if (Synchronization == SynchronizationDevice &&
      Execution != ParentObject.Execution)
    return InvalidParameter;
  if (IsDefault && Device.DefaultQueue)
    return QueueUnsuccessful;
  if (A[4])
    if (auto E = writable(A[4], 8))
      return E;

  Attrs.Parent = A[1];
  auto Handle = createObject(B.Globals, Attrs, false);
  if (!Handle)
    return Handle.takeError();
  Objects.at(*Handle).Kind = ObjectKind::Queue;
  Queue Q;
  Q.Device = A[1];
  Q.Default = Default;
  Q.Read = Read;
  Q.Write = Write;
  Q.DeviceControl = DeviceControl;
  Q.CanceledOnQueue = Read64(QueueConfigCanceled);
  Q.Dispatch = Dispatch;
  if (Dispatch == QueueDispatchParallel)
    Q.PresentedLimit = Read32(QueueConfigPresentedRequests);
  Q.AllowZeroLength = Config[QueueConfigAllowZeroLength] != 0;
  Q.IsDefault = IsDefault;
  Q.PowerManaged = EffectivePowerManagement;
  Q.IoStop = IoStop;
  Q.IoResume = IoResume;
  Queues.emplace(*Handle, Q);
  if (IsDefault)
    Device.DefaultQueue = *Handle;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], *Handle, 8))
      return E;
  return 0;
}

llvm::Expected<bool>
KernelFramework::queueWaitReady(uint64_t Handle, bool IncludePending) const {
  auto Q = Queues.find(Handle);
  if (Q == Queues.end())
    return invalidQueue("synchronous wait lost its queue");
  if (IncludePending && !Q->second.Pending.empty())
    return false;
  if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.Queue == Handle && !Entry.second.Queued &&
               !Entry.second.Completed;
      }))
    return false;
  if (std::any_of(CancelCallbacks.begin(), CancelCallbacks.end(),
                  [&](const auto &Entry) {
                    auto O = Objects.find(Entry.second);
                    return O != Objects.end() && O->second.Parent == Handle;
                  }))
    return false;
  if (std::any_of(CanceledQueueCallbacks.begin(), CanceledQueueCallbacks.end(),
                  [&](const auto &Entry) { return Entry.second == Handle; }))
    return false;
  if (std::any_of(ReadyQueueCallbacks.begin(), ReadyQueueCallbacks.end(),
                  [&](const auto &Entry) { return Entry.second == Handle; }))
    return false;
  return true;
}
} // namespace neverd::emulation

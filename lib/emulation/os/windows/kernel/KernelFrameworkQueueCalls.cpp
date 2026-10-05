//===- KernelFrameworkQueueCalls.cpp - KMDF queue access ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error invalidQueue(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF queue: " + Message);
}
} // namespace

llvm::Expected<KernelFramework::Object *>
KernelFramework::queueObjectForCall(Binding &B, uint64_t Handle,
                                    ObjectKind Kind) {
  auto O = Objects.find(Handle);
  if (O == Objects.end() || O->second.Binding != B.Globals ||
      O->second.Kind != Kind)
    return invalidQueue("invalid, foreign or wrong-kind object handle");
  return &O->second;
}

llvm::Expected<KernelFramework::Queue *>
KernelFramework::queueForCall(Binding &B, uint64_t Handle) {
  auto O = queueObjectForCall(B, Handle, ObjectKind::Queue);
  if (!O)
    return O.takeError();
  auto Q = Queues.find(Handle);
  if (Q == Queues.end() || (**O).Deleting)
    return invalidQueue("queue has no live framework identity");
  return &Q->second;
}

llvm::Expected<KernelFramework::Device *>
KernelFramework::queueDeviceForCall(Binding &B, uint64_t Handle) {
  auto O = queueObjectForCall(B, Handle, ObjectKind::Device);
  if (!O)
    return O.takeError();
  auto D = Devices.find(Handle);
  if (D == Devices.end())
    return invalidQueue("handle has no control-device identity");
  return &D->second;
}

llvm::Expected<uint64_t>
KernelFramework::callQueueReadyNotify(llvm::StringRef, Binding &B,
                                      llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  if (Q.Dispatch != QueueDispatchManual || (A[2] && Q.ReadyNotify) ||
      (!A[2] && (!Q.ReadyNotify || Q.Dispatching)))
    return ControlInvalidDeviceRequest;
  Q.ReadyNotify = A[2];
  Q.ReadyContext = A[2] ? A[3] : 0;
  Q.ReadyPending =
      A[2] && Q.Dispatching && !queuePnpHeld(Q) && !Q.Pending.empty();
  if (auto E = flushReadyNotifications())
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callQueueGetDevice(llvm::StringRef, Binding &B,
                                    llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Object = queueObjectForCall(B, A[1], ObjectKind::Queue);
  if (!Object)
    return Object.takeError();
  auto Q = Queues.find(A[1]);
  if (Q == Queues.end() || !Devices.count(Q->second.Device))
    return invalidQueue("queue lost its owning control device");
  return Q->second.Device;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceGetDefaultQueue(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueDeviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Device = **Selected;
  if (Device.DefaultQueue && !Queues.count(Device.DefaultQueue))
    return invalidQueue("device lost its default-queue identity");
  return Device.DefaultQueue;
}

llvm::Expected<uint64_t> KernelFramework::callDeviceConfigureRequestDispatching(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueDeviceForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Device = **Selected;
  const auto QueueObject = Objects.find(A[2]);
  const auto Target = Queues.find(A[2]);
  if (Objects.at(A[1]).Deleting || QueueObject == Objects.end() ||
      QueueObject->second.Binding != B.Globals ||
      QueueObject->second.Kind != ObjectKind::Queue ||
      QueueObject->second.Deleting || Target == Queues.end() ||
      Target->second.Device != A[1])
    return invalidQueue("dispatch mapping requires a live same-device queue");
  const uint32_t Type = A[3];
  const auto &Queue = Target->second;
  uint64_t Callback = 0;
  switch (Type) {
  case RequestMajorRead:
    Callback = Queue.Read;
    break;
  case RequestMajorWrite:
    Callback = Queue.Write;
    break;
  case RequestMajorDeviceControl:
    Callback = Queue.DeviceControl;
    break;
  case RequestMajorCreate:
  case RequestMajorInternalDeviceControl:
    return invalidQueue("CREATE and internal-control queue mapping requires "
                        "an unmodeled request route");
  default:
    return InvalidParameter;
  }
  if (Queue.IsDefault ||
      (Queue.Dispatch != QueueDispatchManual && !Queue.Default && !Callback))
    return QueueInvalidDeviceRequest;
  if (Device.DispatchQueues.contains(Type))
    return QueueBusy;
  Device.DispatchQueues.emplace(Type, A[2]);
  return 0;
}

} // namespace neverd::emulation

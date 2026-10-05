//===- KernelFrameworkQueueRequests.cpp - KMDF queue requests ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error invalidQueue(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF queue: " + Message);
}
} // namespace

bool KernelFramework::fileBelongsToQueue(Binding &B, uint64_t File,
                                         const Queue &Q) const {
  auto Object = Objects.find(File);
  auto State = FileObjects.find(File);
  return Object != Objects.end() && Object->second.Binding == B.Globals &&
         Object->second.Kind == ObjectKind::File && !Object->second.Deleting &&
         State != FileObjects.end() && State->second.Device == Q.Device;
}

llvm::Expected<std::deque<uint64_t>::iterator>
KernelFramework::findPendingFile(Binding &B, uint64_t QueueHandle, Queue &Q,
                                 std::deque<uint64_t>::iterator Begin,
                                 uint64_t File) {
  for (auto It = Begin; It != Q.Pending.end(); ++It) {
    auto Request = Requests.find(*It);
    auto Object = Objects.find(*It);
    if (Request == Requests.end() || Object == Objects.end() ||
        !Request->second.Queued || Request->second.Queue != QueueHandle ||
        Object->second.Binding != B.Globals ||
        Object->second.Kind != ObjectKind::Request)
      return invalidQueue("queue lost a pending request");
    if (Request->second.File == File)
      return It;
  }
  return Q.Pending.end();
}

llvm::Expected<uint64_t>
KernelFramework::retrievePending(Binding &B, uint64_t QueueHandle, Queue &Q,
                                 std::deque<uint64_t>::iterator Position,
                                 uint64_t Output) {
  const uint64_t Handle = *Position;
  auto Request = Requests.find(Handle);
  auto Object = Objects.find(Handle);
  if (Request == Requests.end() || Object == Objects.end() ||
      !Request->second.Queued || Request->second.Queue != QueueHandle ||
      Object->second.Binding != B.Globals ||
      Object->second.Kind != ObjectKind::Request)
    return invalidQueue("queue lost a pending request");
  if (auto E = Memory.writeInteger(Output, Handle, sizeof(uint64_t)))
    return E;
  Q.Pending.erase(Position);
  if (Q.Pending.empty())
    Q.ReadyPending = false;
  Request->second.Queued = false;
  Request->second.DeliveredOnce = true;
  Request->second.QueuedCallback = 0;
  Request->second.QueuedArguments.clear();
  Request->second.QueuedCompletionStatus.reset();
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callQueueFindRequest(llvm::StringRef Name, Binding &B,
                                      llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  const bool Find = Name == api::WdfIoQueueFindRequest;
  const uint64_t Output = A[Find ? 5 : 3];
  if (auto E = writable(Output, sizeof(uint64_t)))
    return E;
  if (Q.Dispatch != QueueDispatchManual)
    return QueueInvalidDeviceState;
  if (!Q.Dispatching || queuePnpHeld(Q))
    return QueuePaused;
  if (Find && A[3] && !fileBelongsToQueue(B, A[3], Q))
    return invalidQueue("invalid or foreign file-object filter");
  const uint64_t Previous = A[2];
  if (!Find && !Previous)
    return invalidQueue("retrieval requires a live request handle");
  auto PreviousObject = Objects.find(Previous);
  if (Previous && (PreviousObject == Objects.end() ||
                   PreviousObject->second.Binding != B.Globals ||
                   PreviousObject->second.Kind != ObjectKind::Request ||
                   !Requests.count(Previous) ||
                   (Find && !PreviousObject->second.References)))
    return invalidQueue("invalid or unreferenced search request");
  auto Position = Q.Pending.begin();
  if (Previous) {
    Position = std::find(Position, Q.Pending.end(), Previous);
    if (Position == Q.Pending.end()) {
      if (auto E = Memory.writeInteger(Output, 0, sizeof(uint64_t)))
        return E;
      return QueueNotFound;
    }
    if (Find)
      ++Position;
  }
  if (Find && A[3]) {
    auto Match = findPendingFile(B, A[1], Q, Position, A[3]);
    if (!Match)
      return Match.takeError();
    Position = *Match;
  }
  if (Position == Q.Pending.end()) {
    if (auto E = Memory.writeInteger(Output, 0, sizeof(uint64_t)))
      return E;
    return QueueNoMoreEntries;
  }
  if (Find) {
    const uint64_t Handle = *Position;
    auto R = Requests.find(Handle);
    auto O = Objects.find(Handle);
    if (R == Requests.end() || O == Objects.end() || !R->second.Queued ||
        R->second.Queue != A[1] || O->second.Binding != B.Globals ||
        O->second.Kind != ObjectKind::Request)
      return invalidQueue("queue lost a pending request");
    if (O->second.References == UINT64_MAX)
      return invalidQueue("framework reference count overflow");
    if (A[4]) {
      if (!RequestsHost.View)
        return invalidQueue("request inspection host is unavailable");
      auto View = RequestsHost.View(R->second.IRP);
      if (!View)
        return View.takeError();
      if (auto E = writeRequestParameters(A[4], *View))
        return E;
    }
    if (auto E = Memory.writeInteger(Output, Handle, sizeof(uint64_t)))
      return E;
    ++O->second.References;
    return 0;
  }
  return retrievePending(B, A[1], Q, Position, Output);
}

llvm::Expected<uint64_t>
KernelFramework::callQueueRetrieveRequest(llvm::StringRef Name, Binding &B,
                                          llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = queueForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &Q = **Selected;
  const bool ByFile = Name == api::WdfIoQueueRetrieveRequestByFileObject;
  const uint64_t Output = A[ByFile ? 3 : 2];
  if (auto E = writable(Output, sizeof(uint64_t)))
    return E;
  if (Q.Dispatch == QueueDispatchParallel)
    return QueueInvalidDeviceState;
  if (!Q.Dispatching || queuePnpHeld(Q))
    return QueuePaused;
  if (ByFile && !fileBelongsToQueue(B, A[2], Q))
    return invalidQueue(
        "retrieval requires a live file object on the queue device");
  auto Position = Q.Pending.begin();
  if (ByFile) {
    auto Match = findPendingFile(B, A[1], Q, Position, A[2]);
    if (!Match)
      return Match.takeError();
    Position = *Match;
  }
  if (Position == Q.Pending.end()) {
    if (!ByFile) {
      if (auto E = Memory.writeInteger(Output, 0, sizeof(uint64_t)))
        return E;
    }
    return QueueNoMoreEntries;
  }
  return retrievePending(B, A[1], Q, Position, Output);
}

} // namespace neverd::emulation

//===- KernelFrameworkInterruptCalls.cpp - KMDF interrupt calls -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Framework object and power ownership around the shared WDM interrupt
/// authority. Interrupt locks, resource assignment and delivery stay in WDM.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "KernelResources.h"
#include "KernelScheduler.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF: " + Message);
}

} // namespace

llvm::Expected<KernelFramework::Interrupt *>
KernelFramework::interruptForCall(Binding &B, uint64_t Handle) {
  auto Object = Objects.find(Handle);
  auto Entry = InterruptObjects.find(Handle);
  if (Object == Objects.end() || Entry == InterruptObjects.end() ||
      Object->second.Binding != B.Globals || Object->second.Deleting)
    return invalid(
        "interrupt operation requires a live matching framework interrupt");
  return &Entry->second;
}

llvm::Expected<uint64_t>
KernelFramework::callInterruptCreate(llvm::StringRef, Binding &B,
                                     llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  if (IRQL != scheduler::PassiveLevel)
    return invalid(
        "interrupt creation and power callbacks require PASSIVE_LEVEL");
  return createInterrupt(B, A);
}

llvm::Expected<uint64_t>
KernelFramework::callInterruptGetDevice(llvm::StringRef, Binding &B,
                                        llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  return I.Device;
}

llvm::Expected<uint64_t> KernelFramework::callInterruptGetWdmObject(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  return I.Connection ? I.Connection->Token : 0;
}

llvm::Expected<uint64_t>
KernelFramework::callInterruptReportActivity(llvm::StringRef Name, Binding &B,
                                             llvm::ArrayRef<uint64_t> A,
                                             uint8_t IRQL) {
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  if (IRQL > scheduler::DispatchLevel)
    return invalid("interrupt activity reports require IRQL <= DISPATCH_LEVEL");
  if (!I.Connection || !InterruptsHost.SetActive)
    return invalid("interrupt activity reports require a live connection");
  if (I.ChangingState)
    return invalid("interrupt activity report overlaps a state callback");
  if (auto E = InterruptsHost.SetActive(I.Connection->Token,
                                        Name == api::WdfInterruptReportActive))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callInterruptGetInfo(
    llvm::StringRef, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t IRQL) {
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  if (IRQL > scheduler::DispatchLevel)
    return invalid("WdfInterruptGetInfo requires IRQL <= DISPATCH_LEVEL");
  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != InterruptInfoSize)
    return invalid("invalid WDF_INTERRUPT_INFO size");
  if (auto E = writable(A[2], InterruptInfoSize))
    return E;
  if (!Devices.at(I.Device).ResourcesActive)
    return invalid(
        "interrupt information requires assigned hardware resources");
  auto Information = InterruptsHost.Describe(I.Selection);
  if (!Information)
    return Information.takeError();
  const auto Info = Information->value_or(InterruptConnection{});
  std::vector<uint8_t> Empty(InterruptInfoSize);
  if (auto E = Memory.write(A[2], Empty))
    return E;
  struct Field {
    uint64_t Offset, Value;
    unsigned Width;
  };
  for (const auto &F :
       {Field{0, InterruptInfoSize, 4},
        Field{InterruptInfoAffinity, Info.Affinity, 8},
        Field{InterruptInfoMessage, Info.MessageID, 4},
        Field{InterruptInfoVector, Info.Vector, 4},
        Field{InterruptInfoIRQL, Info.IRQL, 1},
        Field{InterruptInfoMode, Info.Mode, 4},
        Field{InterruptInfoPolarity, Info.Polarity, 4},
        Field{InterruptInfoMessageSignaled, Info.Message, 1},
        Field{InterruptInfoShare,
              Info.Share ? uint64_t(DriverInterruptShare::Shared)
                         : resources::DeviceExclusive,
              1}})
    if (auto E = Memory.writeInteger(A[2] + F.Offset, F.Value, F.Width))
      return E;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callInterruptQueueDeferred(
    llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  const bool WorkItem = Name == api::WdfInterruptQueueWorkItemForIsr;
  const uint64_t Routine = WorkItem ? I.WorkItem : I.DPC;
  if (!Routine || !InterruptsHost.QueueDeferred)
    return invalid("interrupt has no requested deferred callback");
  if (!I.Connection || !Devices.at(I.Device).ResourcesActive)
    return invalid(
        "deferred interrupt callback requires assigned connected resources");
  if (NextContinuation == UINT64_MAX ||
      Objects.at(A[1]).InternalReferences == UINT64_MAX)
    return invalid("interrupt callback identity or reference count exhausted");
  const uint64_t Token = NextContinuation;
  auto Queued = InterruptsHost.QueueDeferred(
      A[1], I.Selection.PDO, Routine, {A[1], I.AssociatedObject}, WorkItem,
      Token, I.SynchronizationObject);
  if (!Queued)
    return Queued.takeError();
  if (*Queued) {
    ++NextContinuation;
    ++Objects.at(A[1]).InternalReferences;
    InterruptContinuations.emplace(
        Token, InterruptContinuation{A[1], InterruptCallKind::Deferred});
  }
  return *Queued;
}

llvm::Expected<uint64_t>
KernelFramework::callInterruptLock(llvm::StringRef Name, Binding &B,
                                   llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  if (!I.Connection)
    return invalid("interrupt lock requires a connected interrupt");
  const auto &Operation = Name == api::WdfInterruptAcquireLock
                              ? InterruptsHost.Acquire
                              : InterruptsHost.Release;
  if (!Operation)
    return invalid("interrupt lock host is unavailable");
  if (auto E = Operation(I.Connection->Token))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callInterruptCallback(llvm::StringRef Name, Binding &B,
                                       llvm::ArrayRef<uint64_t> A,
                                       uint8_t IRQL) {
  if (Name != api::WdfInterruptSynchronize) {
    if (IRQL != scheduler::PassiveLevel)
      return invalid(
          "interrupt creation and power callbacks require PASSIVE_LEVEL");
  }
  auto Selected = interruptForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &I = **Selected;
  const bool Synchronize = Name == api::WdfInterruptSynchronize;
  const bool Enabling = Name == api::WdfInterruptEnable;
  if (!I.Connection)
    return invalid("interrupt callback requires a connected interrupt");
  if (I.ChangingState || PendingCall)
    return invalid("interrupt state change is already pending");
  if (!Synchronize && I.Enabled == Enabling)
    return 0;
  const uint64_t Routine = Synchronize ? A[2] : Enabling ? I.Enable : I.Disable;
  if (!Routine) {
    if (Synchronize)
      return invalid("interrupt synchronization requires a callback");
    I.Enabled = Enabling;
    return 0;
  }
  if (NextContinuation == UINT64_MAX)
    return invalid("interrupt callback identity exhausted");
  const uint64_t Token = NextContinuation;
  if (auto E = prepareInterruptCall(Token, A[1], Routine,
                                    {A[1], Synchronize ? A[3] : I.Device}))
    return E;
  ++NextContinuation;
  I.ChangingState = !Synchronize;
  ++Objects.at(A[1]).InternalReferences;
  InterruptContinuations.emplace(
      Token,
      InterruptContinuation{A[1], Synchronize ? InterruptCallKind::Synchronize
                                  : Enabling  ? InterruptCallKind::Enable
                                              : InterruptCallKind::Disable});
  return 0;
}

} // namespace neverd::emulation

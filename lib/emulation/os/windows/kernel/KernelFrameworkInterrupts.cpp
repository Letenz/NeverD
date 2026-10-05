//===- KernelFrameworkInterrupts.cpp - KMDF interrupt lifetimes -----------===//
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

llvm::Expected<uint64_t>
KernelFramework::createInterrupt(Binding &B, llvm::ArrayRef<uint64_t> A) {
  auto Device = Devices.find(A[1]);
  auto DeviceObject = Objects.find(A[1]);
  if (Device == Devices.end() || DeviceObject == Objects.end() ||
      DeviceObject->second.Binding != B.Globals ||
      DeviceObject->second.Deleting)
    return invalid("interrupt creation requires a live framework device");
  auto &D = Device->second;
  if (!D.PDO)
    return invalid("control devices cannot own framework interrupts");
  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size != InterruptConfigSize)
    return InfoLengthMismatch;
  if (auto E = ValidateAccess(A[2], InterruptConfigSize, false))
    return E;
  // Read every field before allocating or publishing an object.
  auto SpinLock = read(A[2] + InterruptSpinLock);
  auto Share = read(A[2] + InterruptShareVector, 4);
  auto Floating = read(A[2] + InterruptFloatingSave, 1);
  auto Automatic = read(A[2] + InterruptAutomaticSerialization, 1);
  auto ISR = read(A[2] + InterruptISR);
  auto DPC = read(A[2] + InterruptDPC);
  auto Enable = read(A[2] + InterruptEnable);
  auto Disable = read(A[2] + InterruptDisable);
  auto WorkItem = read(A[2] + InterruptWorkItem);
  auto Raw = read(A[2] + InterruptRaw);
  auto Translated = read(A[2] + InterruptTranslated);
  auto WaitLock = read(A[2] + InterruptWaitLock);
  auto Passive = read(A[2] + InterruptPassiveHandling, 1);
  auto Inactive = read(A[2] + InterruptReportInactive, 4);
  auto Wake = read(A[2] + InterruptCanWake, 1);
  llvm::Error Errors = llvm::Error::success();
  for (auto *Field :
       {&SpinLock, &Share, &Floating, &Automatic, &ISR, &DPC, &Enable, &Disable,
        &WorkItem, &Raw, &Translated, &WaitLock, &Passive, &Inactive, &Wake})
    if (!*Field)
      Errors = llvm::joinErrors(std::move(Errors), Field->takeError());
  if (Errors)
    return std::move(Errors);
  if (!*ISR || *Share > InterruptTriDefault ||
      *Inactive > InterruptTriDefault || (*DPC && *WorkItem) ||
      (*Passive && *SpinLock) || (!*Passive && *WaitLock))
    return InvalidParameter;
  if (*Wake && (!*Passive || !D.PowerPolicyOwner))
    return InvalidParameter;
  if (*Wake && (!D.Policy.Idle || !D.Policy.Idle->Enabled ||
                !D.Policy.Idle->CanWake || !InterruptsHost.HasPendingWake))
    return invalid("wake interrupts require enabled idle wake policy");
  if (*Inactive == InterruptTriTrue && !InterruptsHost.SetActive)
    return invalid("retained interrupt connection host is unavailable");
  // FloatingSave is ignored by Windows on x64; register state is preserved by
  // the execution backend for both ordinary and interrupt callbacks.
  auto Validation = attributes(A[3], AttributesUse::Object);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return *Status;
  auto Attrs = std::get<Attributes>(*Validation);
  if (Attrs.Parent && Attrs.Parent != A[1]) {
    auto Queue = Queues.find(Attrs.Parent);
    if (Queue == Queues.end() || Queue->second.Device != A[1])
      return ParentAssignmentNotAllowed;
  }
  if (!Attrs.Parent)
    Attrs.Parent = A[1];
  uint64_t SynchronizationObject = 0;
  if (*Automatic) {
    auto Level = executionLevel(Attrs.Parent);
    if (!Level)
      return Level.takeError();
    if ((*DPC && *Level == ExecutionPassive) ||
        (*WorkItem && *Level == ExecutionDispatch))
      return IncompatibleExecutionLevel;
    auto Owner = synchronizationObject(Attrs.Parent);
    if (!Owner)
      return Owner.takeError();
    SynchronizationObject = *Owner;
  }
  const uint64_t ExternalLock = *Passive ? *WaitLock : *SpinLock;
  if (ExternalLock) {
    auto Object = Objects.find(ExternalLock);
    auto Lock = LockObjects.find(ExternalLock);
    if (Object == Objects.end() || Object->second.Binding != B.Globals ||
        Object->second.Deleting || Lock == LockObjects.end() ||
        Lock->second.Wait != bool(*Passive))
      return invalid("interrupt lock must be a live matching framework lock");
    if (Object->second.InternalReferences == UINT64_MAX)
      return invalid("interrupt lock reference count exhausted");
  }

  const bool Preparing = std::any_of(
      PnpTransitions.begin(), PnpTransitions.end(), [&](const auto &Entry) {
        return Entry.second.Device == A[1] &&
               Entry.second.Current.Phase == PnpPhase::PrepareHardware &&
               !Entry.second.CallbacksComplete;
      });
  if ((Preparing && (!*Raw || !*Translated)) ||
      (!Preparing && (*Raw || *Translated || D.HardwarePrepared || D.InD0)))
    return InvalidDeviceState;
  if (*Wake && !Preparing)
    return invalid(
        "wake interrupts require assigned prepare-hardware resources");
  Interrupt Item;
  Item.Device = A[1];
  Item.AssociatedObject = Attrs.Parent;
  Item.ExternalLock = ExternalLock;
  Item.SynchronizationObject = SynchronizationObject;
  Item.ISR = *ISR;
  Item.DPC = *DPC;
  Item.WorkItem = *WorkItem;
  Item.Enable = *Enable;
  Item.Disable = *Disable;
  Item.ReportInactiveOnPowerDown = *Inactive == InterruptTriTrue;
  Item.Selection.PDO = D.PDO;
  Item.Selection.Passive = bool(*Passive);
  Item.Selection.CanWake = bool(*Wake);
  Item.CanWake = bool(*Wake);
  if (ExternalLock) {
    if (*Passive)
      Item.Selection.WaitLock = LockObjects.at(ExternalLock).Storage;
    else
      Item.Selection.SpinLock = LockObjects.at(ExternalLock).Storage;
  }
  if (*Share != InterruptTriDefault)
    Item.Selection.ShareVector = *Share == InterruptTriTrue;
  Item.Selection.Ordinal = std::count_if(
      InterruptObjects.begin(), InterruptObjects.end(),
      [&](const auto &Entry) { return Entry.second.Device == A[1]; });
  if (Preparing) {
    std::optional<uint32_t> Index;
    uint32_t InterruptIndex = 0;
    for (uint32_t I = 0; I < D.RawResources.Count; ++I) {
      const uint64_t Offset = I * resources::ResourceDescriptorSize;
      auto Type = read(D.RawResources.Descriptors + Offset, 1);
      if (!Type)
        return Type.takeError();
      if (D.RawResources.Descriptors + Offset == *Raw &&
          D.TranslatedResources.Descriptors + Offset == *Translated &&
          *Type == resources::InterruptType)
        Index = InterruptIndex;
      if (*Type == resources::InterruptType)
        ++InterruptIndex;
    }
    if (!Index)
      return InvalidParameter;
    Item.Selection.ResourceIndex = *Index;
    Item.Selection.MessageOrdinal =
        std::count_if(InterruptObjects.begin(), InterruptObjects.end(),
                      [&](const auto &Entry) {
                        return Entry.second.Device == A[1] &&
                               Entry.second.Selection.ResourceIndex == Index;
                      });
  }
  if (!InterruptsHost.Connect || !InterruptsHost.Describe ||
      !InterruptsHost.Disconnect || !InterruptsHost.PrepareCall)
    return invalid("framework interrupt host is unavailable");
  if (*Wake) {
    auto Assigned = InterruptsHost.Describe(Item.Selection);
    if (!Assigned)
      return Assigned.takeError();
    if (!*Assigned)
      return invalid("wake interrupt has no assigned hardware resource");
  }
  if (auto E = writable(A[4], 8))
    return E;
  if (auto E = Memory.writeInteger(A[4], 0, 8))
    return E;
  auto Handle = createObject(B.Globals, Attrs, false);
  if (!Handle)
    return Handle.takeError();
  Objects.at(*Handle).Kind = ObjectKind::Interrupt;
  InterruptObjects.emplace(*Handle, std::move(Item));
  if (ExternalLock) {
    ++Objects.at(ExternalLock).InternalReferences;
    LockObjects.at(ExternalLock).InterruptUsers.insert(*Handle);
  }
  if (auto E = Memory.writeInteger(A[4], *Handle, 8))
    return E;
  return windows::StatusSuccess;
}

llvm::Error
KernelFramework::prepareInterruptCall(uint64_t Token, uint64_t Handle,
                                      uint64_t Routine,
                                      llvm::ArrayRef<uint64_t> Arguments) {
  auto I = InterruptObjects.find(Handle);
  if (I == InterruptObjects.end() || !I->second.Connection || PendingCall ||
      !InterruptsHost.PrepareCall || !Routine)
    return invalid("interrupt callback lost its connection or continuation");
  auto Execution = InterruptsHost.PrepareCall(I->second.Connection->Token,
                                              Routine, Arguments, Token);
  if (!Execution)
    return Execution.takeError();
  PendingCall = GuestCall{Token, Routine, Arguments.vec(), *Execution};
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::finishInterruptCallback(uint64_t Token, uint64_t Result) {
  auto Continuation = InterruptContinuations.find(Token);
  if (Continuation == InterruptContinuations.end())
    return invalid("unknown framework interrupt continuation");
  const auto State = Continuation->second;
  auto Object = Objects.find(State.Object);
  auto Interrupt = InterruptObjects.find(State.Object);
  if (Object == Objects.end() || Interrupt == InterruptObjects.end() ||
      !Object->second.InternalReferences)
    return invalid("interrupt callback lost its retained object");
  uint64_t ReturnValue = 0;
  if (State.Kind == InterruptCallKind::Synchronize)
    ReturnValue = uint8_t(Result);
  else if (State.Kind != InterruptCallKind::Deferred) {
    Interrupt->second.ChangingState = false;
    if (uint32_t(Result) & profile::NTStatusFailureMask)
      return invalid("explicit interrupt enable or disable callback failed");
    if (uint32_t(Result) == windows::StatusPending)
      return invalid("interrupt callback returned STATUS_PENDING");
    Interrupt->second.Enabled = State.Kind == InterruptCallKind::Enable;
  }
  --Object->second.InternalReferences;
  InterruptContinuations.erase(Continuation);
  if (auto E = resumePausedPnp())
    return E;
  return PendingCall ? std::optional<uint64_t>{}
                     : std::optional<uint64_t>{ReturnValue};
}

bool KernelFramework::canDeliverWakeInterrupt(uint64_t PDO) const {
  const auto Handle = PnpDeviceHandles.find(PDO);
  if (Handle == PnpDeviceHandles.end())
    return false;
  const auto Device = Devices.find(Handle->second);
  if (Device == Devices.end() || !Device->second.InD0 ||
      !Device->second.ResourcesActive)
    return false;
  return std::any_of(
      PnpTransitions.begin(), PnpTransitions.end(), [&](const auto &Entry) {
        const auto &Transition = Entry.second;
        return Transition.Device == Handle->second &&
               Transition.Current.Phase == PnpPhase::WakeInterrupts &&
               Transition.WaitingForWakeInterrupts &&
               !(Transition.Status & profile::NTStatusFailureMask);
      });
}

bool KernelFramework::hasPendingWakeInterrupts(uint64_t Device) const {
  const auto Entry = Devices.find(Device);
  return Entry != Devices.end() && InterruptsHost.HasPendingWake &&
         InterruptsHost.HasPendingWake(Entry->second.PDO);
}

bool KernelFramework::hasDeferredInterrupts(uint64_t Device) const {
  return std::any_of(InterruptObjects.begin(), InterruptObjects.end(),
                     [&](const auto &Entry) {
                       return Entry.second.Device == Device &&
                              (Objects.at(Entry.first).InternalReferences ||
                               (InterruptsHost.HasDeferred &&
                                InterruptsHost.HasDeferred(Entry.first)));
                     });
}

llvm::Error KernelFramework::disconnectInterrupts(uint64_t Device,
                                                  bool RetainInactive) {
  for (auto &[Handle, I] : InterruptObjects) {
    if (I.Device != Device || !I.Connection)
      continue;
    if (I.ChangingState)
      return invalid("interrupt disconnect overlaps a state callback");
    if (RetainInactive && I.CanWake)
      continue;
    if (RetainInactive && I.ReportInactiveOnPowerDown) {
      if (auto E = InterruptsHost.SetActive(I.Connection->Token, false))
        return E;
    } else {
      if (auto E = InterruptsHost.Disconnect(I.Connection->Token))
        return E;
      I.Connection.reset();
    }
    I.Enabled = false;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> KernelFramework::advancePnpInterrupts(uint64_t Token,
                                                           bool Enable) {
  auto &Transition = PnpTransitions.at(Token);
  for (auto &[Handle, I] : InterruptObjects) {
    if (I.Device != Transition.Device || Handle <= Transition.CurrentInterrupt)
      continue;
    Transition.CurrentInterrupt = Handle;
    if (!Enable && !Transition.ReleasesHardware && I.CanWake)
      continue;
    if (Enable && !I.Connection) {
      if (I.Selection.SpinLock) {
        uint8_t MaximumIRQL = 0;
        for (const auto &[PeerHandle, Peer] : InterruptObjects) {
          if (Peer.ExternalLock != I.ExternalLock ||
              !Devices.at(Peer.Device).ResourcesActive)
            continue;
          auto Info = InterruptsHost.Describe(Peer.Selection);
          if (!Info)
            return Info.takeError();
          if (*Info)
            MaximumIRQL = std::max(MaximumIRQL, (**Info).IRQL);
        }
        I.Selection.SynchronizeIRQL = MaximumIRQL;
      }
      auto Connection = InterruptsHost.Connect(I.Selection, Handle, I.ISR);
      if (!Connection)
        return Connection.takeError();
      I.Connection = *Connection;
    }
    if (!I.Connection || I.Enabled == Enable)
      continue;
    if (Enable && I.ReportInactiveOnPowerDown)
      if (auto E = InterruptsHost.SetActive(I.Connection->Token, true))
        return E;
    const uint64_t Routine = Enable ? I.Enable : I.Disable;
    if (!Routine) {
      I.Enabled = Enable;
      continue;
    }
    if (auto E =
            prepareInterruptCall(Token, Handle, Routine, {Handle, I.Device}))
      return E;
    I.ChangingState = true;
    return true;
  }
  Transition.CurrentInterrupt = 0;
  if (!Enable)
    if (auto E = disconnectInterrupts(Transition.Device,
                                      !Transition.ReleasesHardware))
      return E;
  return false;
}

llvm::Error KernelFramework::finishPnpInterrupt(uint64_t Token,
                                                uint32_t Status) {
  auto &Transition = PnpTransitions.at(Token);
  auto Entry = InterruptObjects.find(Transition.CurrentInterrupt);
  if (Entry == InterruptObjects.end() || !Entry->second.ChangingState)
    return invalid("power interrupt callback lost its state transition");
  auto &I = Entry->second;
  I.ChangingState = false;
  if (!(Status & profile::NTStatusFailureMask))
    I.Enabled = Transition.Current.Phase == PnpPhase::EnableInterrupts;
  return llvm::Error::success();
}
} // namespace neverd::emulation

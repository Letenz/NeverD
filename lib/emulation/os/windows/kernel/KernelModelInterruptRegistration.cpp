//===- KernelModelInterruptRegistration.cpp - WDK interrupt bindings -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Publish and retire decoded interrupt registrations through the shared
/// interrupt authority, preserving resource and storage ownership.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error apiError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "interrupt API: " + Message);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::disconnectInterrupt(uint64_t Object,
                                                          uint32_t Version) {
  if (auto E = Interrupts.disconnect(Object, Version))
    return E;
  if (Version != interrupts::MessageBased &&
      Version != interrupts::MessageBasedPassive)
    FreedRanges.emplace(Object, interrupts::TokenSize);
  return 0;
}

llvm::Expected<uint64_t>
KernelModel::registerInterrupt(InterruptParameters Params) {
  if (!Params.MessageBased && !Params.LineBased && Params.Version &&
      !Params.IRQL && !Params.Synchronize)
    Params.Passive = true;
  if (Params.Passive && (Params.SpinLock || Params.Synchronize))
    return windows::StatusInvalidParameter;
  if (!Params.Output || !Params.Routine)
    return apiError(
        "registration requires output storage and a service routine");
  if (Params.Floating || Params.Group)
    return apiError("only CPU0/group0 interrupts without floating state saving "
                    "are modeled");
  if (!Params.LineBased && !Params.MessageBased && !Params.Affinity)
    return windows::StatusInvalidParameter;
  if (auto E = validateGuestAccess(Params.Output, profile::PointerSize, true))
    return E;
  if (Params.Context)
    if (auto E = validateDispatcherStorage(Params.Context, 1, false))
      return E;
  std::vector<KernelInterrupts::Connection> Candidates;
  if (Params.MessageBased) {
    auto Messages = Interrupts.matchMessages(Params.PDO);
    if (!Messages)
      return Messages.takeError();
    Candidates = std::move(*Messages);
    if (Candidates.empty()) {
      if (!Params.Fallback)
        return windows::StatusNotFound;
      if (auto E = validateGuestAccess(Params.Record, 4, true))
        return E;
      Params.Routine = Params.Fallback;
      Params.MessageBased = false;
      Params.LineBased = true;
      Params.Version = interrupts::LineBased;
    }
  }
  if (!Params.MessageBased) {
    auto Candidate = Interrupts.match(Params.PDO, uint32_t(Params.Vector),
                                      uint8_t(Params.IRQL), Params.Affinity,
                                      Params.LineBased, Params.Passive);
    if (!Candidate)
      return Candidate.takeError();
    if (Params.LineBased && !Candidate->IRQL && !Params.Synchronize)
      Params.Passive = true;
    if (Params.Passive && Params.SpinLock)
      return windows::StatusInvalidParameter;
    if (!Params.Version && !Candidate->IRQL)
      return apiError("passive interrupts require IoConnectInterruptEx");
    if (!Params.Version && Candidate->ResourceMessage)
      return apiError("message interrupts require IoConnectInterruptEx");
    const auto &Resource =
        Resources.find(Candidate->PDO)->Interrupts[Candidate->ResourceIndex];
    if (!Params.LineBased && Params.Mode != uint32_t(Resource.Mode))
      return apiError("interrupt mode must match the assigned resource");
    if (!Params.LineBased &&
        bool(Params.Share) != (Resource.Share == DriverInterruptShare::Shared))
      return apiError("ShareVector must match the assigned interrupt resource");
    if (Params.LineBased && !Params.Synchronize && !Params.Passive)
      Params.Synchronize = Candidate->IRQL;
    if (!Params.Passive &&
        (Params.Synchronize < Candidate->IRQL ||
         Params.Synchronize > DriverInterruptMaximumLevel ||
         (!Params.SpinLock && Params.Synchronize != Candidate->IRQL)))
      return apiError("synchronization IRQL requires the assigned DIRQL or a "
                      "shared caller lock at a higher device DIRQL");
    Candidates.push_back(*Candidate);
  }
  uint8_t UnifiedIRQL = 0;
  if (Params.MessageBased && !Params.Passive) {
    uint8_t MaximumIRQL = 0;
    for (const auto &Candidate : Candidates)
      MaximumIRQL = std::max(MaximumIRQL, Candidate.IRQL);
    if ((Params.Synchronize && Params.Synchronize < MaximumIRQL) ||
        Params.Synchronize > DriverInterruptMaximumLevel)
      return apiError("message synchronization IRQL must cover every message");
    if (Params.SpinLock || Params.Synchronize)
      UnifiedIRQL =
          uint8_t(Params.Synchronize ? Params.Synchronize : MaximumIRQL);
  }
  if (Params.SpinLock) {
    if (Params.SpinLock < profile::UserProbeLimit ||
        Params.SpinLock > UINT64_MAX - profile::PointerSize ||
        Params.SpinLock % profile::PointerSize ||
        (Params.Output < Params.SpinLock + profile::PointerSize &&
         Params.SpinLock < Params.Output + profile::PointerSize))
      return apiError(
          "interrupt spin lock requires separate aligned kernel storage");
    if (ExecutiveSpinLocks.contains(Params.SpinLock))
      return apiError(
          "interrupt spin lock is already owned by an executive lock");
    // A previously connected lock was already checked and is opaque while
    // connected. Its address intentionally names the same lock authority.
    if (!Interrupts.usesSpinLock(Params.SpinLock)) {
      if (auto E = validateDispatcherStorage(Params.SpinLock,
                                             profile::PointerSize, true))
        return E;
      auto Writable =
          Memory.canAccess(Params.SpinLock, profile::PointerSize, Read | Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return apiError(
            "interrupt spin lock requires writable nonpaged storage");
      auto Value = Memory.readInteger(Params.SpinLock, profile::PointerSize);
      if (!Value)
        return Value.takeError();
      if (*Value)
        return apiError("interrupt spin lock must be initialized and free");
    }
  }
  const uint64_t Aligned = (NextAllocation + interrupts::TokenSize - 1) &
                           ~(interrupts::TokenSize - 1);
  const uint64_t TableSize =
      Params.MessageBased ? interrupts::MessageTableHeaderSize +
                                Candidates.size() * interrupts::MessageEntrySize
                          : 0;
  const uint64_t TokenOffset =
      (TableSize + interrupts::TokenSize - 1) & ~(interrupts::TokenSize - 1);
  const uint64_t AllocationSize =
      TokenOffset + Candidates.size() * interrupts::TokenSize;
  if (Aligned > AllocationEnd || AllocationSize > AllocationEnd - Aligned)
    return windows::StatusInsufficientResources;
  for (size_t I = 0; I < Candidates.size(); ++I) {
    auto &Candidate = Candidates[I];
    Candidate.Object = Aligned + TokenOffset + I * interrupts::TokenSize;
    Candidate.Routine = Params.Routine;
    Candidate.Context = Params.Context;
    Candidate.Version = Params.Version;
    Candidate.SpinLock = Params.SpinLock;
    Candidate.Passive = Params.Passive;
    Candidate.SynchronizeIRQL =
        Params.Passive        ? 0
        : Params.MessageBased ? (UnifiedIRQL ? UnifiedIRQL : Candidate.IRQL)
                              : uint8_t(Params.Synchronize);
    for (const auto &[Address, Device] : Devices)
      if (Device.OwnerKind == DeviceOwnerKind::Guest && Device.Extension &&
          Params.Output >= Device.Extension &&
          Params.Output < Address + Device.Size &&
          profile::PointerSize <= Address + Device.Size - Params.Output) {
        if (Device.DeletePending)
          return apiError(
              "interrupt output storage belongs to a deleted device");
        Candidate.OutputDeviceBase = Address;
        Candidate.OutputDeviceSize = Device.Size;
        break;
      }
  }
  if (auto E = Params.MessageBased
                   ? Interrupts.canConnectMessages(Aligned, Candidates)
                   : Interrupts.canConnect(Candidates.front()))
    return E;
  auto Storage = allocate(AllocationSize);
  if (!Storage)
    return Storage.takeError();
  if (Params.MessageBased) {
    if (auto E = Memory.writeInteger(*Storage + interrupts::MessageTableIRQL,
                                     UnifiedIRQL, 1))
      return E;
    if (auto E = Memory.writeInteger(*Storage + interrupts::MessageTableCount,
                                     Candidates.size(), 4))
      return E;
    for (size_t I = 0; I < Candidates.size(); ++I) {
      const auto Message = Interrupts.assignment(Candidates[I]);
      const uint64_t Base = *Storage + interrupts::MessageTableHeaderSize +
                            I * interrupts::MessageEntrySize;
      struct Field {
        uint64_t Offset;
        uint64_t Value;
        unsigned Size;
      };
      const Field Fields[] = {
          {interrupts::MessageAddress, Message.MessageAddress, 8},
          {interrupts::MessageAffinity, Message.TranslatedAffinity, 8},
          {interrupts::MessageObject, Candidates[I].Object, 8},
          {interrupts::MessageData, Message.MessageData, 4},
          {interrupts::MessageVector, Message.TranslatedVector, 4},
          {interrupts::MessageIRQL, Message.TranslatedLevel, 1},
          {interrupts::MessageMode, uint32_t(DriverInterruptMode::Latched), 4},
          {interrupts::MessagePolarity, uint32_t(Message.Polarity), 4}};
      for (const auto &Field : Fields)
        if (auto E = Memory.writeInteger(Base + Field.Offset, Field.Value,
                                         Field.Size))
          return E;
    }
  }
  if (Params.Fallback && !Params.MessageBased)
    if (auto E = Memory.writeInteger(Params.Record, Params.Version, 4))
      return E;
  // All identities and output storage are checked before publishing the group.
  if (auto E =
          Memory.writeInteger(Params.Output, *Storage, profile::PointerSize))
    return E;
  if (auto E = Params.MessageBased
                   ? Interrupts.connectMessages(*Storage, Candidates)
                   : Interrupts.connect(Candidates.front()))
    return E;
  return windows::StatusSuccess;
}
} // namespace neverd::emulation

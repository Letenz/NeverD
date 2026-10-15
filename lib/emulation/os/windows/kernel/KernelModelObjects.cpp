//===- KernelModelObjects.cpp - Object references and handles ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelAPINames.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error objectError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Error KernelModel::validateObjectOutput(uint64_t Address, uint32_t Size) {
  if (auto E = validateGuestAccess(Address, Size, true))
    return E;
  auto Writable = Memory.canAccess(Address, Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return objectError("object output requires writable guest memory");
  return Memory.validateBacking(Address, Size);
}

bool KernelModel::matchesObjectType(ObjectKind Kind, uint64_t Type) const {
  const auto Found = ObjectTypes.find(Kind);
  return !Type || (Found != ObjectTypes.end() && Found->second == Type);
}

llvm::Expected<uint64_t> KernelModel::objectType(uint64_t Object) {
  const auto Found = ReferencedObjects.find(Object);
  if (Found == ReferencedObjects.end())
    return objectError("object type requires a known object");
  if (const auto Type = ObjectTypes.find(Found->second.Kind);
      Type != ObjectTypes.end())
    return Type->second;
  auto Type = allocate(profile::ProcessTokenSize);
  if (!Type)
    return Type.takeError();
  ObjectTypes.emplace(Found->second.Kind, *Type);
  return *Type;
}

llvm::Expected<uint64_t> KernelModel::referenceObject(uint64_t Object) {
  auto Found = ReferencedObjects.find(Object);
  if (Found == ReferencedObjects.end())
    return objectError("object reference requires a known live object");
  ++Found->second.Pointers;
  // ObReferenceObject is a void macro; its underlying export's result is
  // reserved. Do not expose synthetic object-manager header counts.
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::referenceObjectByPointer(llvm::ArrayRef<uint64_t> A) {
  const auto Found = ReferencedObjects.find(A[0]);
  if (Found == ReferencedObjects.end())
    return objectError("pointer reference requires a known live object");
  const uint8_t Mode = A[3];
  if (Mode != windows::KernelMode && Mode != windows::UserMode)
    return objectError("invalid object reference access mode");
  // The target kernel validates a supplied type in both modes. DesiredAccess
  // does not perform an ACL check for a pointer already held by the caller.
  if ((!A[2] && Mode == windows::UserMode) ||
      !matchesObjectType(Found->second.Kind, A[2]))
    return windows::StatusObjectTypeMismatch;
  auto Referenced = referenceObject(A[0]);
  if (!Referenced)
    return Referenced.takeError();
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::dereferenceObject(uint64_t Object) {
  auto Found = ReferencedObjects.find(Object);
  if (Found == ReferencedObjects.end() || !Found->second.Pointers)
    return objectError("ObfDereferenceObject requires a referenced thread or "
                       "process object");
  --Found->second.Pointers;
  retireObjectIfUnreferenced(Object);
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::referenceObjectByHandle(llvm::ArrayRef<uint64_t> A) {
  const auto Handle = ObjectHandles.find(A[0]);
  if (Handle == ObjectHandles.end()) {
    if (Registry.ownsHandle(A[0]))
      return objectError("registry-key object references are unsupported");
    return windows::StatusInvalidHandle;
  }
  const auto &State = Handle->second;
  const auto Found = ReferencedObjects.find(State.Object);
  if (Found == ReferencedObjects.end())
    return objectError("object handle lost its live object");
  const uint8_t Mode = A[3];
  if (Mode != windows::KernelMode && Mode != windows::UserMode)
    return objectError("invalid object reference access mode");
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  const bool Kernel = State.Attributes & windows::ObjectKernelHandle;
  if ((Kernel && Mode == windows::UserMode) ||
      (!Kernel && State.ProcessID != ProcessID))
    return windows::StatusInvalidHandle;
  if (!matchesObjectType(Found->second.Kind, A[2]))
    return windows::StatusObjectTypeMismatch;
  const uint32_t ValidAccess = Found->second.Kind == ObjectKind::Thread
                                   ? windows::ThreadAllAccess
                                   : windows::ProcessAllAccess;
  if (uint32_t(A[1]) & ~ValidAccess)
    return objectError("unsupported object reference access mask");
  if (Mode == windows::UserMode && (uint32_t(A[1]) & ~State.GrantedAccess))
    return windows::StatusAccessDenied;
  if (!A[4])
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(A[4], 8))
    return E;
  if (A[5])
    if (auto E =
            validateObjectOutput(A[5], windows::ObjectHandleInformationSize))
      return E;
  if (auto E = Memory.writeInteger(A[4], State.Object, 8))
    return E;
  if (A[5]) {
    // OBJECT_HANDLE_INFORMATION reports public OBJ_* attributes and the
    // granted mask, not the kernel/user handle-table selection bit.
    if (auto E = Memory.writeInteger(
            A[5], State.Attributes & ~windows::ObjectKernelHandle, 4))
      return E;
    if (auto E = Memory.writeInteger(A[5] + 4, State.GrantedAccess, 4))
      return E;
  }
  ++Found->second.Pointers;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::openObjectByPointer(llvm::ArrayRef<uint64_t> A) {
  const auto Object = ReferencedObjects.find(A[0]);
  if (Object == ReferencedObjects.end())
    return objectError("object open requires a known live object");
  const uint8_t Mode = A[5];
  if (Mode != windows::KernelMode && Mode != windows::UserMode)
    return objectError("invalid object open access mode");
  if ((!A[4] && Mode == windows::UserMode) ||
      !matchesObjectType(Object->second.Kind, A[4]))
    return windows::StatusObjectTypeMismatch;
  // ACLs, access-state records and inheritance need a declared security
  // environment. Refuse those requests without granting invented authority.
  if (Mode != windows::KernelMode || A[2] ||
      (uint32_t(A[1]) & ~windows::ObjectKernelHandle))
    return objectError("object open requires kernel mode, no access state and "
                       "only OBJ_KERNEL_HANDLE attributes");
  const uint32_t ValidAccess = Object->second.Kind == ObjectKind::Thread
                                   ? windows::ThreadAllAccess
                                   : windows::ProcessAllAccess;
  if (uint32_t(A[3]) & ~ValidAccess)
    return objectError("unsupported object open access mask");
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  if (ProcessID != windows::SystemProcessID &&
      !(uint32_t(A[1]) & windows::ObjectKernelHandle))
    return objectError("object open outside the system process requires "
                       "OBJ_KERNEL_HANDLE");
  if (!A[6])
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(A[6], 8))
    return E;
  if (NextObjectHandle > profile::SystemThreadHandleLimit)
    return windows::StatusInsufficientResources;
  if (auto E = Memory.writeInteger(A[6], NextObjectHandle, 8))
    return E;
  retainObjectHandle(A[0], uint32_t(A[3]), uint32_t(A[1]), ProcessID);
  return windows::StatusSuccess;
}

void KernelModel::retainObjectHandle(uint64_t Object, uint32_t Access,
                                     uint32_t Attributes, uint32_t ProcessID) {
  ObjectHandles.emplace(NextObjectHandle,
                        ObjectHandle{Object, Access, Attributes, ProcessID});
  NextObjectHandle += profile::SystemThreadHandleStride;
  ++ReferencedObjects.at(Object).Handles;
}

llvm::Expected<uint64_t> KernelModel::closeHandle(uint64_t Handle) {
  const auto Found = ObjectHandles.find(Handle);
  if (Found == ObjectHandles.end()) {
    if (Registry.ownsHandle(Handle))
      return Registry.call(*this, kernel_api::ZwClose, {Handle});
    return windows::StatusInvalidHandle;
  }
  const auto Context = executionProcessContext();
  const uint32_t ProcessID = Context ? Context->ProcessID : 0;
  if (!(Found->second.Attributes & windows::ObjectKernelHandle) &&
      Found->second.ProcessID != ProcessID)
    return windows::StatusInvalidHandle;
  const uint64_t Object = Found->second.Object;
  auto Reference = ReferencedObjects.find(Object);
  if (Reference == ReferencedObjects.end() || !Reference->second.Handles)
    return objectError("object handle lost its reference");
  --Reference->second.Handles;
  ObjectHandles.erase(Found);
  retireObjectIfUnreferenced(Object);
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::lookupProcess(uint64_t ID,
                                                    uint64_t Output) {
  const bool Known = ID && ID <= UINT32_MAX &&
                     (ID == windows::SystemProcessID ||
                      ProcessObjects.contains(uint32_t(ID)) ||
                      RequestorProcessIDs.contains(uint32_t(ID)) ||
                      (UserRequestContext && ID == CurrentUserProcessID));
  if (!Known)
    return windows::StatusInvalidCID;
  if (!Output)
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(Output, 8))
    return E;
  auto Object = processObject(uint32_t(ID));
  if (!Object)
    return Object.takeError();
  if (auto E = Memory.writeInteger(Output, *Object, 8))
    return E;
  ++ReferencedObjects.at(*Object).Pointers;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::lookupThread(uint64_t ID,
                                                   uint64_t Output) {
  const auto Found =
      std::find_if(ThreadClientIDs.begin(), ThreadClientIDs.end(),
                   [&](const auto &Entry) { return Entry.second == ID; });
  if (Found == ThreadClientIDs.end())
    return windows::StatusInvalidCID;
  if (!Output)
    return windows::StatusInvalidParameter;
  if (auto E = validateObjectOutput(Output, 8))
    return E;
  if (auto E = Memory.writeInteger(Output, Found->first, 8))
    return E;
  ++ReferencedObjects.at(Found->first).Pointers;
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::threadProcess(uint64_t Object) {
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end() || !Thread->second.ProcessID)
    return objectError(
        "thread process requires a known thread process identity");
  return processObject(Thread->second.ProcessID);
}

llvm::Expected<uint64_t> KernelModel::threadTerminating(uint64_t Object) const {
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end())
    return objectError(
        "thread termination query requires a known thread object");
  return uint64_t(Thread->second.Terminating || Thread->second.Exited);
}

void KernelModel::retireObjectIfUnreferenced(uint64_t Object) {
  const auto References = ReferencedObjects.find(Object);
  if (References == ReferencedObjects.end() || References->second.Pointers ||
      References->second.Handles || WaitReferences.contains(Object))
    return;
  // Process identities are currently retained by the declared request/session
  // environment. Dropping a caller reference must not revoke that environment.
  if (References->second.Kind == ObjectKind::Process)
    return;
  const auto Thread = ThreadObjects.find(Object);
  if (Thread == ThreadObjects.end() || !Thread->second.Exited)
    return;
  FreedRanges.emplace(Object, profile::ProcessTokenSize);
  if (Thread->second.System)
    Scheduler.forgetThreadPriority(Thread->second.CallbackID);
  ThreadClientIDs.erase(Object);
  ThreadObjects.erase(Thread);
  ReferencedObjects.erase(References);
}

void KernelModel::retireBorrowedThread(uint64_t Key) {
  ApcStates.erase(Key);
  SystemAffinityThreads.erase(Key);
  if (const auto Current = CurrentThreadObjects.find(Key);
      Current != CurrentThreadObjects.end()) {
    const uint64_t Object = Current->second;
    auto &Thread = ThreadObjects.at(Object);
    Thread.Exited = true;
    Thread.ExitPriority = Scheduler.threadPriority(Key);
    CurrentThreadObjects.erase(Current);
    retireObjectIfUnreferenced(Object);
  }
  // A reused stack key starts a new thread. Retained old objects use their
  // captured exit priority and cannot affect the new thread's scheduler state.
  Scheduler.forgetThreadPriority(Key);
}
} // namespace neverd::emulation

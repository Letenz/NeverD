//===- KernelFrameworkRequestBufferCalls.cpp - KMDF request calls -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error requestError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF request: " + Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelFramework::callRequestMemoryBuffer(llvm::StringRef, Binding &B,
                                         llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto O = Objects.find(A[1]);
  auto M = RequestMemories.find(A[1]);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Memory ||
      O->second.Binding != B.Globals || M == RequestMemories.end() ||
      !M->second.Active)
    return requestError("invalid or completed framework request memory");
  if (A[2]) {
    if (auto E = writable(A[2], sizeof(uint64_t)))
      return E;
    if (auto E = Memory.writeInteger(A[2], M->second.Length, sizeof(uint64_t)))
      return E;
  }
  return M->second.Buffer;
}

llvm::Expected<uint64_t> KernelFramework::callRequestRetrieveMemory(
    llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto View = requestViewForCall(R);
  if (!View)
    return View.takeError();
  const bool InputMemory = Name == api::WdfRequestRetrieveInputMemory;
  const bool OutputMemory = Name == api::WdfRequestRetrieveOutputMemory;
  if (auto E = writable(A[2], sizeof(uint64_t)))
    return E;
  if (auto E = Memory.writeInteger(A[2], 0, sizeof(uint64_t)))
    return E;
  if (View->Neither || (InputMemory && View->Major == RequestMajorRead) ||
      (OutputMemory && View->Major == RequestMajorWrite))
    return ControlInvalidDeviceRequest;
  const uint32_t Length = OutputMemory ? View->OutputLength : View->InputLength;
  if (!Length)
    return RequestBufferTooSmall;
  auto Existing = std::find_if(
      RequestMemories.begin(), RequestMemories.end(), [&](const auto &Entry) {
        const auto &Memory = Entry.second;
        return Memory.Request == A[1] && Memory.Active &&
               Memory.Output == OutputMemory;
      });
  if (Existing != RequestMemories.end()) {
    if (auto E = Memory.writeInteger(A[2], Existing->first, sizeof(uint64_t)))
      return E;
    return 0;
  }
  if (!RequestsHost.Buffer)
    return requestError("request buffer host is unavailable");
  auto Buffer = RequestsHost.Buffer(R.IRP, OutputMemory);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return windows::StatusInsufficientResources;
  Attributes Attrs;
  Attrs.Parent = A[1];
  auto Created = createObject(B.Globals, Attrs, false);
  if (!Created)
    return Created.takeError();
  const uint64_t Handle = *Created;
  Objects.at(Handle).Kind = ObjectKind::Memory;
  RequestMemories.emplace(Handle, RequestMemory{A[1], std::nullopt, *Buffer,
                                                Length, true, OutputMemory});
  if (auto E = Memory.writeInteger(A[2], Handle, sizeof(uint64_t)))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callRequestRetrieveUnsafeBuffer(
    llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto View = requestViewForCall(R);
  if (!View)
    return View.takeError();
  const bool UnsafeInput = Name == api::WdfRequestRetrieveUnsafeUserInputBuffer;
  const bool UnsafeOutput =
      Name == api::WdfRequestRetrieveUnsafeUserOutputBuffer;
  if (auto E = writable(A[3], sizeof(uint64_t)))
    return E;
  if (A[4])
    if (auto E = writable(A[4], sizeof(uint64_t)))
      return E;
  if (auto E = Memory.writeInteger(A[3], 0, sizeof(uint64_t)))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], 0, sizeof(uint64_t)))
      return E;
  if (!R.InCallerContext || !View->Neither ||
      (UnsafeInput && View->Major == RequestMajorRead) ||
      (UnsafeOutput && View->Major == RequestMajorWrite))
    return ControlInvalidDeviceRequest;
  const uint32_t Length = UnsafeOutput ? View->OutputLength : View->InputLength;
  const uint64_t Buffer = UnsafeOutput ? View->UserOutput : View->UserInput;
  if (!Buffer || !Length || Length < A[2])
    return RequestBufferTooSmall;
  if (auto E = Memory.writeInteger(A[3], Buffer, sizeof(uint64_t)))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], Length, sizeof(uint64_t)))
      return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestProbeBuffer(llvm::StringRef Name, Binding &B,
                                        llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto View = requestViewForCall(R);
  if (!View)
    return View.takeError();
  const bool ProbeWrite = Name == api::WdfRequestProbeAndLockUserBufferForWrite;
  if (auto E = writable(A[4], sizeof(uint64_t)))
    return E;
  if (auto E = Memory.writeInteger(A[4], 0, sizeof(uint64_t)))
    return E;
  if (!R.InCallerContext)
    return RequestAccessViolation;
  if (!RequestsHost.ProbeAndLock || !RequestsHost.ReleaseUserBuffer)
    return requestError("user-buffer lock host is unavailable");
  auto Locked = RequestsHost.ProbeAndLock(R.IRP, A[2], A[3], ProbeWrite);
  if (!Locked)
    return Locked.takeError();
  if (Locked->Status)
    return Locked->Status;
  Attributes Attrs;
  Attrs.Parent = A[1];
  auto Handle = createObject(B.Globals, Attrs, false);
  if (!Handle)
    return llvm::joinErrors(Handle.takeError(),
                            RequestsHost.ReleaseUserBuffer(Locked->MDL));
  Objects.at(*Handle).Kind = ObjectKind::Memory;
  RequestMemories.emplace(
      *Handle, RequestMemory{A[1], Locked->MDL, Locked->Buffer, A[3]});
  if (auto E = Memory.writeInteger(A[4], *Handle, sizeof(uint64_t)))
    return E;
  return 0;
}

llvm::Expected<uint64_t>
KernelFramework::callRequestGetParameters(llvm::StringRef, Binding &B,
                                          llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto View = requestViewForCall(R);
  if (!View)
    return View.takeError();
  if (auto E = writeRequestParameters(A[2], *View))
    return E;
  return 0;
}

llvm::Expected<uint64_t> KernelFramework::callRequestRetrieveBuffer(
    llvm::StringRef Name, Binding &B, llvm::ArrayRef<uint64_t> A, uint8_t) {
  auto Selected = requestForCall(B, A[1]);
  if (!Selected)
    return Selected.takeError();
  auto &R = **Selected;
  auto View = requestViewForCall(R);
  if (!View)
    return View.takeError();
  if (auto E = writable(A[3], 8))
    return E;
  if (A[4])
    if (auto E = writable(A[4], 8))
      return E;
  if (auto E = Memory.writeInteger(A[3], 0, 8))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], 0, 8))
      return E;
  const bool Output = Name == api::WdfRequestRetrieveOutputBuffer;
  if (View->Neither)
    return ControlInvalidDeviceRequest;
  if ((!Output && View->Major == RequestMajorRead) ||
      (Output && View->Major == RequestMajorWrite))
    return ControlInvalidDeviceRequest;
  const auto Length = Output ? View->OutputLength : View->InputLength;
  if (!Length || Length < A[2])
    return RequestBufferTooSmall;
  if (!RequestsHost.Buffer)
    return requestError("request buffer host is unavailable");
  auto Buffer = RequestsHost.Buffer(R.IRP, Output);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return windows::StatusInsufficientResources;
  if (auto E = Memory.writeInteger(A[3], *Buffer, 8))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], Length, 8))
      return E;
  return 0;
}

} // namespace neverd::emulation

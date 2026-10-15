//===- KernelModelFileObjects.cpp - Retained WDM file bodies --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Caller references retain a file body independently of scenario identities.

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <utility>

namespace neverd::emulation {
namespace {
llvm::Error fileError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Error KernelModel::retireFileIfUnreferenced(uint64_t Object) {
  const auto Found = FileObjects.find(Object);
  const auto References = ReferencedObjects.find(Object);
  if (Found == FileObjects.end() || References == ReferencedObjects.end())
    return fileError("file retirement lost its object-manager identity");
  if (References->second.Pointers || References->second.Handles)
    return llvm::Error::success();
  auto &File = Found->second;
  if (File.State == FileState::Failed) {
    FreedRanges.emplace(Object, windows::FileSize);
    FileObjects.erase(Found);
    ReferencedObjects.erase(References);
    return llvm::Error::success();
  }
  if (!File.DeferredClose)
    return llvm::Error::success();
  if (PendingWdmCall || PendingWait)
    return fileError("file CLOSE requires the preceding continuation to start");
  const auto Process = executionProcessContext();
  if (!Process)
    return fileError(
        "file CLOSE requires the releasing thread's process context");
  auto Close = std::move(*File.DeferredClose);
  File.DeferredClose.reset();
  Close.Input.RequestorProcessID = Process->ProcessID;
  Result.Requests[Close.ResultIndex].RequestorProcessID =
      Close.Input.RequestorProcessID;
  auto Call = beginFileRequest(Close.Input, Close.SourceIndex,
                               Close.ResultIndex, Object);
  if (!Call)
    return Call.takeError();
  if (!Call->IRP)
    return fileError("final file release did not create its CLOSE request");
  if (FinalizedRequests.contains(Call->IRP))
    return llvm::Error::success();
  InternalFileCloses.insert(Call->IRP);
  if (!Call->PC) {
    if (auto E = tryFinalizeFileClose(Call->IRP))
      return E;
    return waitForFileClose(Call->IRP);
  }
  if (Call->FrameworkCallerContext)
    return fileError("file CLOSE cannot enter framework caller-context I/O");
  const uint64_t Token = NextIRPCall++;
  IRPCall Continuation{IRPCallKind::FileClose, Call->IRP, 0, true};
  Continuation.FrameworkDispatchStatus = Call->FrameworkDispatchStatus;
  IRPCalls.emplace(Token, std::move(Continuation));
  std::vector<uint64_t> Arguments{Call->Argument0, Call->Argument1,
                                  Call->Argument2, Call->Argument3};
  Arguments.insert(Arguments.end(), Call->StackArguments.begin(),
                   Call->StackArguments.end());
  PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                   Call->PC,
                                   std::move(Arguments),
                                   Call->SynchronizationObject};
  return llvm::Error::success();
}

llvm::Error KernelModel::waitForFileClose(uint64_t IRP) {
  if (!InternalFileCloses.contains(IRP))
    return llvm::Error::success();
  if (PendingWait)
    return fileError("file CLOSE cannot replace an existing caller wait");
  Wait Pending;
  Pending.Type = Wait::Kind::FileClose;
  Pending.Object = IRP;
  Pending.Execution = CurrentExecution;
  Pending.Thread = CurrentThreadKey;
  Pending.IRQL = CurrentIRQL;
  PendingWait = std::move(Pending);
  return llvm::Error::success();
}

llvm::Error KernelModel::tryFinalizeFileClose(uint64_t IRP) {
  const auto *Request = requestForIRP(IRP);
  if (!InternalFileCloses.contains(IRP) || !Request || !Request->Completed ||
      !Request->DispatchReturned)
    return llvm::Error::success();
  if (std::any_of(IRPCalls.begin(), IRPCalls.end(),
                  [&](const auto &Entry) { return Entry.second.IRP == IRP; }))
    return llvm::Error::success();
  if (auto E = finalizeRequest(IRP))
    return E;
  InternalFileCloses.erase(IRP);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishFileCloseDispatch(uint64_t Token, uint64_t ResultValue) {
  const auto &Call = IRPCalls.at(Token);
  const uint64_t IRP = Call.IRP;
  if (auto E = recordDispatchReturn(
          IRP, Call.FrameworkDispatchStatus.value_or(uint32_t(ResultValue))))
    return E;
  IRPCalls.erase(Token);
  if (auto E = tryFinalizeFileClose(IRP))
    return E;
  if (auto E = waitForFileClose(IRP))
    return E;
  // ObfDereferenceObject has a reserved return, not the driver's NTSTATUS.
  return std::optional<uint64_t>{0};
}
} // namespace neverd::emulation

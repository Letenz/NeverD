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

bool KernelModel::owesFileClose(const OpenFile &File) const {
  if (File.State == FileState::Failed || File.State == FileState::Closing)
    return false;
  return File.DeferredClose.has_value() ||
         File.Source == OpenFile::Origin::DriverCreate;
}

void KernelModel::associateFileRequest(DriverRequestResult &Observation,
                                       const OpenFile &File) const {
  Observation.FileObject = File.Address;
  if (File.Source == OpenFile::Origin::DriverCreate)
    Observation.FileOpenOperation = File.CreateOperation;
  if (File.ProviderOpen)
    if (const auto *Open = FileSystem.findOpen(*File.ProviderOpen)) {
      Observation.FileSystemDevice = Open->DeviceObject;
      Observation.FileSystemFileID = Open->FileID;
    }
}

llvm::Error KernelModel::validateFileSystemOpenBindings() const {
  std::vector<KernelFileSystem::OpenBinding> Bindings;
  for (const auto &[Object, File] : FileObjects)
    if (File.ProviderOpen) {
      if (File.Source != OpenFile::Origin::DriverCreate ||
          File.State == FileState::Failed || !File.CreateOperation)
        return fileError("provider open lost its live driver-created file");
      Bindings.push_back({Object, *File.ProviderOpen});
    }
  return FileSystem.validateOpenBindings(Bindings);
}

llvm::Error
KernelModel::retireFileIfUnreferenced(uint64_t Object,
                                      std::optional<uint32_t> CallerProcess) {
  const auto Found = FileObjects.find(Object);
  const auto References = ReferencedObjects.find(Object);
  if (Found == FileObjects.end() || References == ReferencedObjects.end())
    return fileError("file retirement lost its object-manager identity");
  if (References->second.Pointers || References->second.Handles)
    return llvm::Error::success();
  auto &File = Found->second;
  if (File.HandleCloseHolds || File.InternalIOHolds)
    return llvm::Error::success();
  if (File.State == FileState::Failed) {
    if (File.ProviderOpen || owesFileClose(File))
      return fileError("failed file still owns provider teardown");
    if (auto E = prepareReleaseRange(Object, windows::FileSize))
      return E;
    if (File.NameStorage) {
      if (auto E = prepareReleaseRange(File.NameStorage, File.NameStorageSize))
        return E;
      FreedRanges.emplace(File.NameStorage, File.NameStorageSize);
    }
    FreedRanges.emplace(Object, windows::FileSize);
    FileObjects.erase(Found);
    ReferencedObjects.erase(References);
    return llvm::Error::success();
  }
  if (!owesFileClose(File))
    return llvm::Error::success();
  if (PendingWdmCall || PendingWait)
    return fileError("file CLOSE requires the preceding continuation to start");
  const auto Process = executionProcessContext();
  if (!CallerProcess && !Process)
    return fileError(
        "file CLOSE requires the releasing thread's process context");
  DeferredFileClose Close;
  if (File.DeferredClose) {
    Close = std::move(*File.DeferredClose);
  } else {
    if (File.State != FileState::Cleaned)
      return fileError("automatic CLOSE requires completed CLEANUP");
    Close.Input.Kind = DriverRequestKind::Close;
    DriverRequestResult Observation;
    Observation.Kind = DriverRequestKind::Close;
    Observation.Origin = DriverRequestOrigin::FileFinalClose;
    associateFileRequest(Observation, File);
    Close.ResultIndex = Result.Requests.size();
    Result.Requests.push_back(std::move(Observation));
  }
  File.DeferredClose.reset();
  Close.Input.RequestorProcessID =
      CallerProcess ? *CallerProcess : Process->ProcessID;
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
                                   Call->SynchronizationObject,
                                   CallerProcess};
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

llvm::Error KernelModel::validateFileHandleClose(uint64_t Object,
                                                 bool Scenario) const {
  const auto File = FileObjects.find(Object);
  if (File == FileObjects.end() || (File->second.State != FileState::Open &&
                                    File->second.State != FileState::Cleaned))
    return fileError("file handle close requires an opened file body");
  if (CurrentIRQL != scheduler::PassiveLevel ||
      (!Scenario && !executionProcessContext()))
    return fileError("file handle close requires a known PASSIVE_LEVEL caller");
  if (PendingWdmCall || PendingWait || NextIRPCall == UINT64_MAX ||
      NextFileHandleClose == UINT64_MAX)
    return fileError("file handle close cannot replace a pending continuation");
  return llvm::Error::success();
}

llvm::Expected<std::optional<DriverRequestKind>>
KernelModel::planFileHandleClose(uint64_t Handle) const {
  const auto Found = ObjectHandles.find(Handle);
  if (Found == ObjectHandles.end())
    return fileError("file handle close lost its handle");
  const auto &State = Found->second;
  if (auto E = validateFileHandleClose(State.Object, State.ScenarioOwned))
    return E;
  const auto &References = ReferencedObjects.at(State.Object);
  if (References.Kind != ObjectKind::File || !References.Handles)
    return fileError("file handle close lost its file reference");
  const size_t ProcessHandles = std::count_if(
      ObjectHandles.begin(), ObjectHandles.end(), [&](const auto &Entry) {
        return Entry.second.Object == State.Object &&
               Entry.second.ProcessID == State.ProcessID;
      });
  const size_t Total = References.Handles;
  if (ProcessHandles == 1 &&
      std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == State.Object;
      }))
    return fileError(
        "file handle callback with outstanding file I/O is unsupported");
  if (ProcessHandles != 1)
    return std::optional<DriverRequestKind>{};
  return std::optional<DriverRequestKind>{
      Total == 1 ? DriverRequestKind::Cleanup : DriverRequestKind::UnlockAll};
}

void KernelModel::releaseFileHandle(uint64_t Handle) {
  --ReferencedObjects.at(ObjectHandles.at(Handle).Object).Handles;
  ObjectHandles.erase(Handle);
}

llvm::Expected<KernelModel::Invocation>
KernelModel::beginFileHandleRequest(uint64_t Object, DriverRequestKind Kind,
                                    uint32_t ProcessID) {
  auto &File = FileObjects.at(Object);
  DriverRequest Input;
  Input.Kind = Kind;
  Input.File = File.ScenarioID;
  Input.RequestorProcessID = ProcessID;
  size_t Index;
  std::optional<size_t> Source;
  if (Kind == DriverRequestKind::Cleanup && File.DeferredCleanup) {
    const auto &Deferred = *File.DeferredCleanup;
    Input = Deferred.Input;
    Input.RequestorProcessID = ProcessID;
    Index = Deferred.ResultIndex;
    Source = Deferred.SourceIndex;
    Result.Requests[Index].RequestorProcessID = ProcessID;
  } else {
    DriverRequestResult Observation;
    Observation.Kind = Kind;
    Observation.Origin = DriverRequestOrigin::FileHandleClose;
    Observation.File = File.ScenarioID;
    Observation.RequestorProcessID = ProcessID;
    associateFileRequest(Observation, File);
    Index = Result.Requests.size();
    Result.Requests.push_back(std::move(Observation));
  }
  auto Call = beginFileRequest(Input, Source, Index, Object);
  if (Call && Kind == DriverRequestKind::Cleanup)
    File.DeferredCleanup.reset();
  return Call;
}

llvm::Error KernelModel::dispatchFileHandleClose(uint64_t ID,
                                                 const Invocation &Call,
                                                 uint32_t ProcessID) {
  if (!Call.IRP)
    return fileError("file handle callback did not create an IRP");
  FileHandleCloses.at(ID).IRP = Call.IRP;
  if (FinalizedRequests.contains(Call.IRP))
    return llvm::Error::success();
  InternalFileCloses.insert(Call.IRP);
  if (!Call.PC) {
    if (auto E = tryFinalizeFileClose(Call.IRP))
      return E;
    return FinalizedRequests.contains(Call.IRP) ? llvm::Error::success()
                                                : waitForFileHandleClose(ID);
  }
  if (Call.FrameworkCallerContext)
    return fileError("file handle callback cannot enter caller-context I/O");
  const uint64_t Token = NextIRPCall++;
  IRPCall Continuation{IRPCallKind::FileHandleClose, Call.IRP, 0, true};
  Continuation.FrameworkDispatchStatus = Call.FrameworkDispatchStatus;
  Continuation.FileHandleOperation = ID;
  IRPCalls.emplace(Token, std::move(Continuation));
  std::vector<uint64_t> Arguments{Call.Argument0, Call.Argument1,
                                  Call.Argument2, Call.Argument3};
  Arguments.insert(Arguments.end(), Call.StackArguments.begin(),
                   Call.StackArguments.end());
  PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                   Call.PC,
                                   std::move(Arguments),
                                   Call.SynchronizationObject,
                                   ProcessID};
  return llvm::Error::success();
}

llvm::Error KernelModel::closeFileHandle(uint64_t Handle) {
  const auto State = ObjectHandles.at(Handle);
  const auto Context = executionProcessContext();
  auto Kind = planFileHandleClose(Handle);
  if (!Kind)
    return Kind.takeError();
  if (!*Kind) {
    releaseFileHandle(Handle);
    return llvm::Error::success();
  }
  // Admit and prepare the actual packet before consuming the caller's handle.
  // No guest callback executes until the returned continuation is scheduled.
  auto Call = beginFileHandleRequest(State.Object, **Kind, State.ProcessID);
  if (!Call)
    return Call.takeError();
  releaseFileHandle(Handle);
  const uint64_t ID = NextFileHandleClose++;
  ++FileObjects.at(State.Object).HandleCloseHolds;
  FileHandleCloses.emplace(
      ID, FileHandleClose{State.Object, 0, Context->ProcessID});
  if (auto E = dispatchFileHandleClose(ID, *Call, State.ProcessID))
    return E;
  if (FinalizedRequests.contains(Call->IRP)) {
    auto Continued = resumeFileHandleClose(ID);
    if (!Continued)
      return Continued.takeError();
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::waitForFileHandleClose(uint64_t ID) {
  if (PendingWait)
    return fileError("file handle close cannot replace an existing wait");
  Wait Pending;
  Pending.Type = Wait::Kind::FileHandleClose;
  Pending.Object = ID;
  Pending.Execution = CurrentExecution;
  Pending.Thread = CurrentThreadKey;
  Pending.IRQL = CurrentIRQL;
  PendingWait = std::move(Pending);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::resumeFileHandleClose(uint64_t ID) {
  const auto Found = FileHandleCloses.find(ID);
  if (Found == FileHandleCloses.end() ||
      !FinalizedRequests.contains(Found->second.IRP))
    return fileError(
        "file handle continuation requires completed callback I/O");
  const auto Operation = Found->second;
  --FileObjects.at(Operation.File).HandleCloseHolds;
  FileHandleCloses.erase(Found);
  if (auto E =
          retireFileIfUnreferenced(Operation.File, Operation.CallerProcess))
    return E;
  if (PendingWdmCall)
    return std::optional<uint64_t>{};
  return std::optional<uint64_t>{windows::StatusSuccess};
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishFileHandleDispatch(uint64_t Token, uint64_t ReturnValue) {
  const auto Call = IRPCalls.at(Token);
  if (auto E = recordDispatchReturn(
          Call.IRP,
          Call.FrameworkDispatchStatus.value_or(uint32_t(ReturnValue))))
    return E;
  IRPCalls.erase(Token);
  if (auto E = tryFinalizeFileClose(Call.IRP))
    return E;
  if (FinalizedRequests.contains(Call.IRP))
    return resumeFileHandleClose(Call.FileHandleOperation);
  if (auto E = waitForFileHandleClose(Call.FileHandleOperation))
    return E;
  return std::optional<uint64_t>{windows::StatusSuccess};
}
} // namespace neverd::emulation

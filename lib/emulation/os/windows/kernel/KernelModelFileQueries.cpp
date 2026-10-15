//===- KernelModelFileQueries.cpp - Caller-owned file query continuations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Produce query IRPs with borrowed output and resume the original API caller
/// after dispatch, completion and any final FILE_OBJECT close have returned.

#include "KernelContextCapture.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <utility>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint64_t QueryStatusSize = 16;
constexpr uint64_t QueryStorageSize = QueryStatusSize + dispatcher::EventSize;
constexpr unsigned QueryMajor = 5;

llvm::Error queryError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "file information query: " + Text);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::relatedFileDevice(uint64_t Object) const {
  const auto Found = FileObjects.find(Object);
  if (Found == FileObjects.end() || Found->second.State == FileState::Failed ||
      Found->second.State == FileState::Closing)
    return queryError("related device requires a live file object");
  const auto &File = Found->second;
  auto Type = Memory.readInteger(Object, 2);
  auto Size = Memory.readInteger(Object + 2, 2);
  if (!Type || !Size)
    return llvm::joinErrors(Type.takeError(), Size.takeError());
  if (*Type != FileType || *Size != FileSize)
    return queryError("FILE_OBJECT header differs from its owner");
  auto Device = Memory.readInteger(Object + FileDeviceOffset, 8);
  auto VPB = Memory.readInteger(Object + FileVPBOffset, 8);
  auto Extension = Memory.readInteger(Object + FileExtensionOffset, 8);
  if (!Device || !VPB || !Extension)
    return llvm::joinErrors(
        Device.takeError(),
        llvm::joinErrors(VPB.takeError(), Extension.takeError()));
  if (*Device != File.Device || !Devices.contains(*Device))
    return queryError("FILE_OBJECT lost its authoritative device identity");
  auto DeviceVPB = Memory.readInteger(*Device + DeviceVPBOffset, 8);
  if (!DeviceVPB)
    return DeviceVPB.takeError();
  if (*VPB || *DeviceVPB || *Extension)
    return queryError("mounted VPB and file-extension routing are unsupported");
  if (auto E = validateDeviceTopology())
    return E;
  return topAttachedDevice(*Device);
}

std::optional<uint64_t> KernelModel::takeFileQueryContinuation() {
  return std::exchange(PendingFileQueryContinuation, std::nullopt);
}

llvm::Expected<uint64_t>
KernelModel::queryFileInformation(llvm::ArrayRef<uint64_t> A,
                                  const KernelContextCapture *Context) {
  const auto Process = executionProcessContext();
  if (!Process || Process->RestrictedAttachment ||
      CurrentIRQL != scheduler::PassiveLevel)
    return queryError("requires a known PASSIVE_LEVEL caller");
  if (auto E = validateFileQueryInterrupts(Context))
    return E;
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return queryError("requires enabled special kernel APCs");
  if (Unloading || Unloaded || PendingWdmCall || PendingWait ||
      PendingFileQueryContinuation || NextFileInformationQuery == UINT64_MAX ||
      NextIRPCall == UINT64_MAX)
    return queryError("cannot replace an outstanding continuation");
  const auto Found = FileObjects.find(A[0]);
  if (Found == FileObjects.end() || (Found->second.State != FileState::Open &&
                                     Found->second.State != FileState::Cleaned))
    return queryError("requires an opened file object");
  auto &File = Found->second;
  if (File.InternalIOHolds == UINT32_MAX)
    return queryError("file I/O hold capacity exhausted");
  if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == A[0];
      }))
    return queryError("same-file I/O must finalize before a query");
  const uint32_t Class = uint32_t(A[1]), Length = uint32_t(A[2]);
  if (Length > profile::KernelArenaSize)
    return queryError("buffer exceeds the 1 MiB model operation limit");
  if (Length)
    if (auto E = validateObjectOutput(A[3], Length))
      return E;
  if (auto E = validateObjectOutput(A[4], 4))
    return E;
  auto Flags = Memory.readInteger(A[0] + FileFlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (bool(*Flags & FileSynchronousIO) == File.Asynchronous)
    return queryError("FILE_OBJECT synchronous mode differs from its owner");
  if (auto E = snapshot())
    return E;
  auto Top = relatedFileDevice(A[0]);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  if (auto E = validateFileSystemRoute(*Route))
    return E;
  auto Count = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!Count)
    return Count.takeError();
  if (!*Count || *Count > MaxIRPStackCount || *Count < Route->size())
    return queryError("device requires a positive bounded IRP stack count");
  if (File.PnpDevice) {
    if (*Top == File.PnpDevice)
      return queryError("provider PDO has no guest query dispatch");
    if (auto E = Lifecycle.validateIoSubmission(File.PnpDevice))
      return E;
  }
  const uint64_t Callback = Result.MajorFunctions[QueryMajor];
  if (!Callback &&
      anyInitialized(DispatchBytesWritten, DispatchInitialization,
                     QueryMajor * profile::PointerSize, profile::PointerSize))
    return queryError("driver explicitly registered a null query callback");
  auto Thread = currentThreadObject();
  if (!Thread)
    return Thread.takeError();
  // The synchronous file lock resets its event before IRP allocation. A
  // resource failure leaves that reset visible without changing ReturnedLength.
  if (!File.Asynchronous) {
    const uint64_t FileEvent = File.Address + FileEventOffset;
    File.QueryEventInitialized = true;
    auto Initialized =
        Dispatcher.isWaitable(FileEvent)
            ? Dispatcher.call("KeResetEvent", {FileEvent}, CurrentIRQL,
                              CurrentThreadKey)
            : Dispatcher.call("KeInitializeEvent", {FileEvent, 0, 0},
                              CurrentIRQL, CurrentThreadKey);
    if (!Initialized)
      return Initialized.takeError();
  }
  const uint64_t PacketSize = IRPSize + *Count * StackSize;
  const auto Align = [](uint64_t Value) {
    return (Value + PoolAlignment - 1) & ~(PoolAlignment - 1);
  };
  const uint64_t Start = Align(NextAllocation);
  if (Start > AllocationEnd || PacketSize > AllocationEnd - Start ||
      Align(Start + PacketSize) > AllocationEnd ||
      QueryStorageSize > AllocationEnd - Align(Start + PacketSize))
    return StatusInsufficientResources;
  auto Packet = allocate(PacketSize, PoolAlignment);
  if (!Packet)
    return Packet.takeError();
  auto Storage = allocate(QueryStorageSize, PoolAlignment);
  if (!Storage)
    return Storage.takeError();
  const uint64_t ID = NextFileInformationQuery++;
  FileInformationQuery Operation;
  Operation.File = A[0];
  Operation.IRP = *Packet;
  Operation.ReturnedLength = A[4];
  Operation.Storage = *Storage;
  Operation.CallerExecution = CurrentExecution;
  Operation.CallerThread = CurrentThreadKey;
  Operation.CallerProcess = Process->ProcessID;
  FileInformationQueries.emplace(ID, Operation);
  ++File.InternalIOHolds;
  DriverRequestResult Observation;
  Observation.Kind = DriverRequestKind::QueryInformation;
  Observation.Origin = DriverRequestOrigin::FileInformationQuery;
  Observation.Device = Devices.at(File.Device).Name;
  Observation.File = File.ScenarioID;
  Observation.RequestorProcessID = Process->ProcessID;
  Observation.FileInformationClass = Class;
  Observation.IRP = *Packet;
  if (const auto *Owner = pnpDeviceForPDO(File.PnpDevice))
    Observation.DeviceID = Result.PnpDevices[Owner->ResultIndex].ID;
  const size_t Index = Result.Requests.size();
  Result.Requests.push_back(std::move(Observation));
  ActiveRequest Record{DriverRequestKind::QueryInformation, Index};
  Record.IRP = *Packet;
  Record.Device = File.Device;
  Record.FileAddress = File.Address;
  Record.FileId = File.ScenarioID;
  Record.PnpDevice = File.PnpDevice;
  Record.LifecycleIo = File.PnpDevice != 0;
  Record.StackCount = *Count;
  Record.Stack = *Packet + IRPSize + (*Count - 1) * StackSize;
  Record.DeviceRoute = std::move(*Route);
  Record.UnwoundPending.resize(*Count);
  Record.SystemBuffer = A[3];
  Record.BorrowedSystemBuffer = true;
  Record.FileQueryOperation = ID;
  Record.BufferSize = Length;
  Record.OutputSize = Record.TransferSize = Length;
  Record.AsynchronousFile = File.Asynchronous;
  Record.ProcessID = Process->ProcessID;
  Record.CallbackProcessID = Process->ProcessID;
  auto &Request = Requests.emplace(*Packet, std::move(Record)).first->second;
  if (Request.LifecycleIo)
    if (auto E = Lifecycle.trackIo(Request.PnpDevice, *Packet))
      return E;
  for (uint64_t Owner : Request.DeviceRoute)
    if (auto E = retainDevice(Owner))
      return E;
  if (auto E = initializeIRPHeader(*Packet, *Count, *Count))
    return E;
  const uint64_t Event = File.Asynchronous ? *Storage + QueryStatusSize : 0;
  if (Event) {
    auto Initialized = Dispatcher.call("KeInitializeEvent", {Event, 1, 0},
                                       CurrentIRQL, CurrentThreadKey);
    if (!Initialized)
      return Initialized.takeError();
  }
  struct Field {
    uint64_t Address, Value;
    unsigned Width;
  };
  for (const Field &F : std::array<Field, 14>{
           {{*Packet + IRPFlagsOffset, File.Asynchronous ? 0x14u : 0x10u, 4},
            {*Packet + IRPAllocationFlagsOffset, File.Asynchronous ? 0u : 2u,
             1},
            {*Packet + IRPSystemBufferOffset, A[3], 8},
            {*Packet + IRPRequestorModeOffset, KernelMode, 1},
            {*Packet + IRPOriginalFileOffset, File.Address, 8},
            {*Packet + IRPThreadOffset, *Thread, 8},
            {*Packet + IRPUserIosbOffset, *Storage, 8},
            {*Packet + IRPUserEventOffset, Event, 8},
            {Request.Stack, QueryMajor, 1},
            {Request.Stack + StackFlagsOffset, Class == 75 ? 1u : 0u, 1},
            {Request.Stack + StackQueryLengthOffset, Length, 4},
            {Request.Stack + StackQueryClassOffset, Class == 75 ? 71 : Class,
             4},
            {Request.Stack + StackDeviceOffset, *Top, 8},
            {Request.Stack + StackFileOffset, File.Address, 8}}})
    if (auto E = Memory.writeInteger(F.Address, F.Value, F.Width))
      return E;
  auto Watch = watchInitialization(*Packet + IRPStatusOffset,
                                   Request.IOStatusWritten.size());
  if (!Watch)
    return Watch.takeError();
  Request.IOStatusInitialization = std::move(*Watch);
  if (Framework) {
    auto Routed = Framework->routeRequest(*Top, *Packet);
    if (!Routed)
      return Routed.takeError();
    if (*Routed)
      return queryError("framework query dispatch is unsupported");
  }
  if (!File.Asynchronous) {
    auto Entered = enterApcRegion(ApcRegionKind::Critical);
    if (!Entered)
      return Entered.takeError();
    FileInformationQueries.at(ID).CriticalRegion = true;
  }
  if (Callback) {
    const uint64_t Token = NextIRPCall++;
    IRPCall Continuation{IRPCallKind::FileInformationQuery, *Packet, 0, true};
    Continuation.FileQueryOperation = ID;
    IRPCalls.emplace(Token, std::move(Continuation));
    PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                     Callback,
                                     {*Top, *Packet},
                                     0,
                                     Process->ProcessID};
  } else {
    if (auto E = Memory.writeInteger(*Packet + IRPStatusOffset,
                                     InvalidDeviceRequest, 4))
      return E;
    Request.IOStatusWritten.fill(true);
    if (auto E = completeRequest(*Packet, 0))
      return E;
    if (auto E = recordDispatchReturn(*Packet, InvalidDeviceRequest))
      return E;
  }
  PendingFileQueryContinuation = ID;
  return 0; // The session must retain the caller until resume publishes it.
}

llvm::Error
KernelModel::validateFileQueryInterrupts(const KernelContextCapture *Context) {
  if (!Context)
    return llvm::Error::success(); // Direct model calls use the default
                                   // context.
  auto Captured = Context->Read();
  if (!Captured)
    return Captured.takeError();
  const auto Flags = llvm::support::endian::read32le(Captured->Bytes.data() +
                                                     seh::ContextEFlagsOffset);
  if (!(Flags & 0x200))
    return queryError("interrupt-disabled query execution is unsupported");
  return llvm::Error::success();
}

llvm::Error KernelModel::publishFileQueryCompletion(uint64_t IRP) {
  auto &Request = Requests.at(IRP);
  auto &Query = FileInformationQueries.at(Request.FileQueryOperation);
  if (Query.CompletionPublished)
    return llvm::Error::success();
  // IopfCompleteRequest queues requestor APC delivery when a worker or an
  // elevated/guarded caller completes the packet. Eligible API and execution
  // boundaries deliver the notification without retiring the query operation.
  if (CurrentThreadKey != Query.CallerThread ||
      CurrentIRQL != scheduler::PassiveLevel)
    return llvm::Error::success();
  if (auto Process = executionProcessContext();
      !Process || Process->RestrictedAttachment ||
      Process->ProcessID != Query.CallerProcess)
    return llvm::Error::success();
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return llvm::Error::success();
  auto &Observation = Result.Requests[Request.ResultIndex];
  const uint32_t Status = *Observation.IOStatus;
  // Completion on a worker never reads the borrowed caller address. Capture
  // report bytes only after restoring its owning process, before length aliases
  // are overwritten by the final API continuation.
  if ((Status >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity &&
      Observation.Information) {
    if (auto E = validateGuestAccess(Request.SystemBuffer,
                                     Observation.Information, false))
      return E;
    Observation.Output.resize(Observation.Information);
    if (auto E = Memory.read(Request.SystemBuffer, Observation.Output))
      return E;
  }
  if ((Status >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity ||
      Request.PendingMarked) {
    if (auto E = Memory.writeInteger(Query.Storage, Status, 4))
      return E;
    if (auto E =
            Memory.writeInteger(Query.Storage + 8, Observation.Information, 8))
      return E;
    const uint64_t Event = Request.AsynchronousFile
                               ? Query.Storage + QueryStatusSize
                               : Query.File + FileEventOffset;
    auto Signaled = Dispatcher.call("KeSetEvent", {Event, 0, 0}, CurrentIRQL,
                                    CurrentThreadKey);
    if (!Signaled)
      return Signaled.takeError();
    if (!Request.AsynchronousFile)
      if (auto E = Memory.writeInteger(Query.File + FileFinalStatusOffset,
                                       Status, 4))
        return E;
  }
  Query.CompletionPublished = true;
  return llvm::Error::success();
}

llvm::Error
KernelModel::deliverFileQueryCompletions(const KernelContextCapture *Context) {
  for (const auto &[ID, Query] : FileInformationQueries) {
    (void)ID;
    if (Query.CompletionPublished || Query.Released ||
        Query.CallerThread != CurrentThreadKey)
      continue;
    const auto *Request = requestForIRP(Query.IRP);
    if (!Request || !Request->Completed)
      continue;
    if (Context) {
      auto Captured = Context->Read();
      if (!Captured)
        return Captured.takeError();
      if (!(llvm::support::endian::read32le(Captured->Bytes.data() +
                                            seh::ContextEFlagsOffset) &
            0x200))
        return llvm::Error::success();
    }
    if (auto E = publishFileQueryCompletion(Query.IRP))
      return E;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> KernelModel::fileInformationQueryReady(uint64_t ID) const {
  const auto Found = FileInformationQueries.find(ID);
  if (Found == FileInformationQueries.end() || Found->second.Released)
    return queryError("wait lost its active query operation");
  const auto *Request = requestForIRP(Found->second.IRP);
  if (!Request)
    return queryError("operation lost its query IRP");
  return Request->Completed && Request->DispatchReturned &&
         std::none_of(IRPCalls.begin(), IRPCalls.end(), [&](const auto &Entry) {
           return Entry.second.IRP == Found->second.IRP;
         });
}

llvm::Error KernelModel::waitForFileInformationQuery(uint64_t ID) {
  if (PendingWait)
    return queryError("cannot replace an existing wait");
  const auto &Query = FileInformationQueries.at(ID);
  Wait Pending;
  Pending.Type = Wait::Kind::FileInformationQuery;
  Pending.Object = ID;
  Pending.Execution = Query.CallerExecution;
  Pending.Thread = Query.CallerThread;
  Pending.IRQL = scheduler::PassiveLevel;
  PendingWait = std::move(Pending);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishFileInformationDispatch(uint64_t Token,
                                           uint64_t ReturnValue) {
  const auto Call = IRPCalls.at(Token);
  if (auto E = recordDispatchReturn(Call.IRP, uint32_t(ReturnValue)))
    return E;
  IRPCalls.erase(Token);
  auto Ready = fileInformationQueryReady(Call.FileQueryOperation);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    if (auto E = waitForFileInformationQuery(Call.FileQueryOperation))
      return E;
  return std::optional<uint64_t>{0}; // Never publish from the dispatch frame.
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::resumeFileInformationQuery(uint64_t ID) {
  const auto Found = FileInformationQueries.find(ID);
  if (Found == FileInformationQueries.end())
    return queryError("continuation lost its operation");
  auto &Query = Found->second;
  const auto Process = executionProcessContext();
  if (CurrentExecution != Query.CallerExecution ||
      CurrentThreadKey != Query.CallerThread || !Process ||
      Process->ProcessID != Query.CallerProcess ||
      CurrentIRQL != scheduler::PassiveLevel || PendingWdmCall || PendingWait)
    return queryError("continuation requires the original caller context");
  if (!Query.Released) {
    auto Ready = fileInformationQueryReady(ID);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready) {
      if (auto E = waitForFileInformationQuery(ID))
        return E;
      return std::optional<uint64_t>{};
    }
    if (auto E = publishFileQueryCompletion(Query.IRP))
      return E;
    if (!Query.CompletionPublished)
      return queryError("caller cannot deliver its query completion APC");
    const auto &Observation =
        Result.Requests[Requests.at(Query.IRP).ResultIndex];
    auto Information = Memory.readInteger(Query.Storage + 8, 8);
    if (!Information)
      return Information.takeError();
    Query.Status = *Observation.DispatchStatus == StatusPending
                       ? *Observation.IOStatus
                       : *Observation.DispatchStatus;
    Query.Information = uint32_t(*Information);
    if (auto E = validateObjectOutput(Query.ReturnedLength, 4))
      return E;
    if (auto E = prepareReleaseRange(Query.Storage, QueryStorageSize))
      return E;
    if (auto E = finalizeRequest(Query.IRP))
      return E;
    FreedRanges.emplace(Query.Storage, QueryStorageSize);
    --FileObjects.at(Query.File).InternalIOHolds;
    Query.Released = true;
    if (auto E = retireFileIfUnreferenced(Query.File, Query.CallerProcess))
      return E;
    if (PendingWdmCall || PendingWait)
      return std::optional<uint64_t>{};
  }
  // Completion or final CLOSE can have run on another execution. Validate at
  // the publication point, including aliases into the borrowed output buffer.
  if (Query.CriticalRegion) {
    auto Left = leaveApcRegion(ApcRegionKind::Critical);
    if (!Left)
      return Left.takeError();
    Query.CriticalRegion = false;
  }
  if (auto E = validateObjectOutput(Query.ReturnedLength, 4))
    return E;
  if (auto E = Memory.writeInteger(Query.ReturnedLength, Query.Information, 4))
    return E;
  const uint64_t Status = Query.Status;
  FileInformationQueries.erase(Found);
  return std::optional<uint64_t>{Status};
}
} // namespace neverd::emulation

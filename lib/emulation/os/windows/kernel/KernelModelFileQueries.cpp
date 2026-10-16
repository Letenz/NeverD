//===- KernelModelFileQueries.cpp - Caller-owned file query continuations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Produce borrowed or buffered query IRPs and resume the original API caller
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
KernelModel::queryFileInformation(llvm::ArrayRef<uint64_t> A, bool Native,
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
      PendingFileQueryContinuation || PendingFileOperation ||
      NextFileInformationQuery == UINT64_MAX || NextIRPCall == UINT64_MAX)
    return queryError("cannot replace an outstanding continuation");
  const uint64_t StatusOutput = Native ? A[1] : 0;
  uint32_t GrantedAccess = 0;
  std::array<uint64_t, 5> Normalized;
  if (Native) {
    const auto *Handle = findObjectHandle(A[0]);
    if (!Handle)
      return StatusInvalidHandle;
    const auto Object = ReferencedObjects.find(Handle->Object);
    if (Object == ReferencedObjects.end())
      return queryError("handle lost its live object");
    if (Object->second.Kind != ObjectKind::File)
      return StatusObjectTypeMismatch;
    // Zw selects KernelMode for object referencing. The handle's process scope
    // still applies, but its granted mask is not a kernel-mode access check.
    GrantedAccess = Handle->GrantedAccess;
    Normalized = {Handle->Object, uint32_t(A[4]), uint32_t(A[3]), A[2], 0};
    A = Normalized;
  }
  const auto Found = FileObjects.find(A[0]);
  if (Found == FileObjects.end() || (Found->second.State != FileState::Open &&
                                     Found->second.State != FileState::Cleaned))
    return queryError("requires an opened file object");
  auto &File = Found->second;
  const uint32_t Class = uint32_t(A[1]), Length = uint32_t(A[2]);
  auto Flags = Memory.readInteger(A[0] + FileFlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (bool(*Flags & FileSynchronousIO) == File.Asynchronous)
    return queryError("FILE_OBJECT synchronous mode differs from its owner");
  // Access and remote-device observations do not take the file lock. The
  // synchronous position fast path does, even though it never creates an IRP.
  if ((!Native || (Class != 8 && Class != 51)) &&
      std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == A[0];
      }))
    return queryError("same-file I/O must finalize before a query");
  if (Native &&
      (Class == 8 || Class == 51 || (Class == 14 && !File.Asynchronous))) {
    const unsigned Width = Class == 51 ? 1 : Class == 14 ? 8 : 4;
    if (Length < Width)
      return StatusInfoLengthMismatch;
    if (auto E = snapshot())
      return E;
    if (auto E = validateObjectOutput(A[3], Width))
      return E;
    if (auto E = validateObjectOutput(StatusOutput, QueryStatusSize))
      return E;
    uint64_t Value = GrantedAccess;
    if (Class != 8) {
      auto Top = relatedFileDevice(File.Address);
      if (!Top)
        return Top.takeError();
      auto Field = Memory.readInteger(
          Class == 14 ? File.Address + FileCurrentByteOffset
                      : File.Device + DeviceCharacteristicsOffset,
          Class == 14 ? 8 : 4);
      if (!Field)
        return Field.takeError();
      Value = Class == 14 ? *Field : bool(*Field & 0x10);
    }
    if (auto E = Memory.writeInteger(A[3], Value, Width))
      return E;
    if (auto E = Memory.writeInteger(StatusOutput, StatusSuccess, 4))
      return E;
    if (auto E = Memory.writeInteger(StatusOutput + 8, Width, 8))
      return E;
    return StatusSuccess;
  }
  if (Native) {
    switch (Class) {
    case 4:
    case 5:
    case 6:
    case 7:
    case 9:
    case 14:
    case 16:
    case 17:
    case 21:
    case 22:
    case 28:
    case 34:
    case 35:
    case 59:
    case 71:
    case 75:
      break;
    default:
      return queryError("native information class is not implemented");
    }
    if (!Length || ((Class == 16 || Class == 17) && Length < 4))
      return queryError("native buffered query requires a valid class buffer");
  }
  if (File.InternalIOHolds == UINT32_MAX)
    return queryError("file I/O hold capacity exhausted");
  if (Length > profile::KernelArenaSize)
    return queryError("buffer exceeds the 1 MiB model operation limit");
  if (Length)
    if (auto E = validateObjectOutput(A[3], Length))
      return E;
  if (auto E = validateObjectOutput(Native ? StatusOutput : A[4],
                                    Native ? QueryStatusSize : 4))
    return E;
  if (auto E = snapshot())
    return E;
  auto Top = relatedFileDevice(A[0]);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  if (auto E = validateFileSystemRoute(*Route, File.Address,
                                       DriverRequestKind::QueryInformation))
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
  const bool Provider = FileSystem.ownsDevice(*Top);
  const bool LocalNative = Native && (Class == 16 || Class == 17);
  if (Native && !Provider && !File.Asynchronous && (Class == 4 || Class == 5)) {
    auto FastIO = Memory.readInteger(
        Devices.at(*Top).OwnerDriver + DriverFastIOOffset, 8);
    if (!FastIO)
      return FastIO.takeError();
    if (*FastIO)
      return queryError("native Fast I/O queries are not implemented");
  }
  const uint64_t Callback = Provider ? 0 : Result.MajorFunctions[QueryMajor];
  if (!Provider && !LocalNative && !Callback &&
      anyInitialized(DispatchBytesWritten, DispatchInitialization,
                     QueryMajor * profile::PointerSize, profile::PointerSize))
    return queryError("driver explicitly registered a null query callback");
  auto Thread = currentThreadObject();
  if (!Thread)
    return Thread.takeError();
  // The synchronous file lock resets its event before IRP allocation. A
  // resource failure leaves that reset visible without changing ReturnedLength.
  if (!File.Asynchronous || Native) {
    const uint64_t FileEvent = File.Address + FileEventOffset;
    File.FileEventInitialized = true;
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
      QueryStorageSize > AllocationEnd - Align(Start + PacketSize) ||
      (Native &&
       (Align(Align(Start + PacketSize) + QueryStorageSize) > AllocationEnd ||
        Length > AllocationEnd -
                     Align(Align(Start + PacketSize) + QueryStorageSize))))
    return StatusInsufficientResources;
  auto Packet = allocate(PacketSize, PoolAlignment);
  if (!Packet)
    return Packet.takeError();
  auto Storage = allocate(QueryStorageSize, PoolAlignment);
  if (!Storage)
    return Storage.takeError();
  uint64_t Buffer = A[3];
  if (Native) {
    auto Owned = allocatePhysicalBuffer(Length, PoolAlignment, 0, Length);
    if (!Owned)
      return Owned.takeError();
    Buffer = *Owned;
    if (auto E = Memory.write(Buffer, std::vector<uint8_t>(Length)))
      return E;
  }
  const uint64_t ID = NextFileInformationQuery++;
  FileInformationQuery Operation;
  Operation.File = A[0];
  Operation.IRP = *Packet;
  Operation.ReturnedLength = A[4];
  Operation.Native = Native;
  Operation.StatusOutput = StatusOutput;
  Operation.Storage = *Storage;
  Operation.CallerExecution = CurrentExecution;
  Operation.CallerThread = CurrentThreadKey;
  Operation.CallerProcess = Process->ProcessID;
  FileInformationQueries.emplace(ID, Operation);
  ++File.InternalIOHolds;
  DriverRequestResult Observation;
  Observation.Kind = DriverRequestKind::QueryInformation;
  Observation.Origin = Native ? DriverRequestOrigin::FileHandleQuery
                              : DriverRequestOrigin::FileInformationQuery;
  Observation.Device = Devices.at(File.Device).Name;
  Observation.File = File.ScenarioID;
  Observation.RequestorProcessID = Process->ProcessID;
  Observation.FileInformationClass = Class;
  Observation.IRP = *Packet;
  associateFileRequest(Observation, File);
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
  Record.SystemBuffer = Buffer;
  Record.BorrowedSystemBuffer = !Native;
  Record.UserBuffer = Native ? A[3] : 0;
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
  for (const Field &F : std::array<Field, 15>{
           {{*Packet + IRPFlagsOffset,
             (File.Asynchronous ? 0x14u : 0x10u) | (Native ? 0x860u : 0u), 4},
            {*Packet + IRPAllocationFlagsOffset, File.Asynchronous ? 0u : 2u,
             1},
            {*Packet + IRPSystemBufferOffset, Buffer, 8},
            {*Packet + IRPUserBufferOffset, Native ? A[3] : 0, 8},
            {*Packet + IRPRequestorModeOffset, KernelMode, 1},
            {*Packet + IRPOriginalFileOffset, File.Address, 8},
            {*Packet + IRPThreadOffset, *Thread, 8},
            {*Packet + IRPUserIosbOffset,
             Native && !File.Asynchronous ? StatusOutput : *Storage, 8},
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
  if (Native && (Class == 16 || Class == 17)) {
    auto Value =
        Class == 17
            ? Memory.readInteger(*Top + DeviceAlignmentOffset, 4)
            : llvm::Expected<uint64_t>(
                  ((*Flags & 0x10) ? 2u : 0u) | ((*Flags & 0x20) ? 4u : 0u) |
                  ((*Flags & 8) ? 8u : 0u) |
                  ((*Flags & 2) ? ((*Flags & 4) ? 0x10u : 0x20u) : 0u) |
                  ((*Flags & 0x10000) ? 0x1000u : 0u));
    if (!Value)
      return Value.takeError();
    if (auto E = Memory.writeInteger(Buffer, *Value, 4))
      return E;
    if (auto E = Memory.writeInteger(*Packet + IRPStatusOffset, 0, 4))
      return E;
    if (auto E = Memory.writeInteger(*Packet + IRPInformationOffset, 4, 8))
      return E;
    Request.IOStatusWritten.fill(true);
    if (auto E = completeRequest(*Packet, 0))
      return E;
    if (auto E = recordDispatchReturn(*Packet, 0))
      return E;
  } else if (Provider) {
    auto Status = dispatchFileSystem(*Top, *Packet);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(*Packet, uint32_t(*Status)))
      return E;
  } else if (Callback) {
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
  // IRP_DEFER_IO_COMPLETION leaves an immediate native query intact until the
  // producing wrapper regains control. Pending packets notify the requestor
  // through the same eligible-thread boundary as borrowed queries.
  if (Query.Native && !Request.PendingMarked && !Request.DispatchReturned)
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
    if (Query.Native) {
      if (auto E =
              validateObjectOutput(Request.UserBuffer, Observation.Information))
        return E;
      if (auto E = Memory.write(Request.UserBuffer, Observation.Output))
        return E;
    }
  }
  const bool NativeLocalStatus =
      Query.Native && Request.AsynchronousFile && Request.PendingMarked;
  const uint64_t StatusBlock =
      Query.Native && !NativeLocalStatus ? Query.StatusOutput : Query.Storage;
  if (Query.Native && !Request.PendingMarked) {
    if (auto E =
            Memory.writeInteger(IRP + IRPUserIosbOffset, Query.StatusOutput, 8))
      return E;
    if (auto E = Memory.writeInteger(IRP + IRPUserEventOffset, 0, 8))
      return E;
  }
  if ((Status >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity ||
      Request.PendingMarked) {
    if (Query.Native && !NativeLocalStatus)
      if (auto E = validateObjectOutput(StatusBlock, QueryStatusSize))
        return E;
    // The completion owner publishes Information before the four-byte Status;
    // an aliased output has already been copied by this point.
    if (auto E =
            Memory.writeInteger(StatusBlock + 8, Observation.Information, 8))
      return E;
    if (auto E = Memory.writeInteger(StatusBlock, Status, 4))
      return E;
    const bool LocalEvent =
        Request.AsynchronousFile && (!Query.Native || NativeLocalStatus);
    const uint64_t Event = LocalEvent ? Query.Storage + QueryStatusSize
                                      : Query.File + FileEventOffset;
    auto Signaled = Dispatcher.call("KeSetEvent", {Event, 0, 0}, CurrentIRQL,
                                    CurrentThreadKey);
    if (!Signaled)
      return Signaled.takeError();
    if (!LocalEvent)
      if (auto E = Memory.writeInteger(Query.File + FileFinalStatusOffset,
                                       Status, 4))
        return E;
  }
  Query.CompletionPublished = true;
  if (Query.Native && Request.Completed)
    return releaseNativeFileQueryPacket(IRP);
  return llvm::Error::success();
}

bool KernelModel::nativeFileQuery(uint64_t ID) const {
  return ID && FileInformationQueries.at(ID).Native;
}

llvm::Error KernelModel::releaseNativeFileQueryPacket(uint64_t IRP) {
  const auto &Request = Requests.at(IRP);
  auto &Query = FileInformationQueries.at(Request.FileQueryOperation);
  if (!Query.Native || Query.PacketReleased || !Query.CompletionPublished)
    return llvm::Error::success();
  if (!Request.Completed)
    return queryError("native disposal requires completed stack unwinding");
  const uint64_t PacketSize = IRPSize + Request.StackCount * StackSize;
  if (auto E = prepareReleaseRanges(
          {{Request.SystemBuffer, Request.BufferSize}, {IRP, PacketSize}}))
    return E;
  if (auto E = Physical.retire(Request.SystemBuffer))
    return E;
  FreedRanges.emplace(Request.SystemBuffer, Request.BufferSize);
  FreedRanges.emplace(IRP, PacketSize);
  Query.PacketReleased = true;
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
    Query.Status = *Observation.DispatchStatus == StatusPending
                       ? *Observation.IOStatus
                       : *Observation.DispatchStatus;
    if (Query.Native) {
      Query.CopyStatusBlock = Requests.at(Query.IRP).AsynchronousFile &&
                              *Observation.DispatchStatus == StatusPending;
      if (Query.CopyStatusBlock)
        if (auto E = Memory.read(Query.Storage, Query.StatusBytes))
          return E;
    } else {
      auto Information = Memory.readInteger(Query.Storage + 8, 8);
      if (!Information)
        return Information.takeError();
      Query.Information = uint32_t(*Information);
      if (auto E = validateObjectOutput(Query.ReturnedLength, 4))
        return E;
    }
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
  if (Query.CopyStatusBlock) {
    if (auto E = validateObjectOutput(Query.StatusOutput, QueryStatusSize))
      return E;
    if (auto E = Memory.write(Query.StatusOutput, Query.StatusBytes))
      return E;
  } else if (!Query.Native) {
    if (auto E = validateObjectOutput(Query.ReturnedLength, 4))
      return E;
    if (auto E =
            Memory.writeInteger(Query.ReturnedLength, Query.Information, 4))
      return E;
  }
  const uint64_t Status = Query.Status;
  FileInformationQueries.erase(Found);
  return std::optional<uint64_t>{Status};
}
} // namespace neverd::emulation

//===- driver_wdm_owned_irp.c - Genuine WDK caller-owned IRPs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Submit independently allocated internal requests to a separate private
/// device. The allocating driver retains storage ownership through inline,
/// worker and cancel completions, including nested completion of an outer IRP.
///
//===----------------------------------------------------------------------===//
#include "driver_wdm_owned_irp_test.h"

#include <ntddk.h>

#define ABI_OFFSET(Type, Member, Offset)                                       \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset, #Type "." #Member)
_Static_assert(sizeof(IRP) == 0xd0, "x64 IRP ABI");
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x48, "x64 stack ABI");
ABI_OFFSET(IRP, Type, 0);
ABI_OFFSET(IRP, Size, 2);
ABI_OFFSET(IRP, MdlAddress, 8);
ABI_OFFSET(IRP, IoStatus, 0x30);
_Static_assert(sizeof(IO_STATUS_BLOCK) == 16, "x64 I/O status ABI");
_Static_assert((CCHAR)-1 < 0, "signed stack count ABI");
ABI_OFFSET(IRP, RequestorMode, 0x40);
ABI_OFFSET(IRP, PendingReturned, 0x41);
ABI_OFFSET(IRP, StackCount, 0x42);
ABI_OFFSET(IRP, CurrentLocation, 0x43);
ABI_OFFSET(IRP, Tail.Overlay.Thread, 0x98);
ABI_OFFSET(IRP, Tail.Overlay.CurrentStackLocation, 0xb8);
ABI_OFFSET(IO_STACK_LOCATION, DeviceObject, 0x28);
_Static_assert(IRP_MJ_INTERNAL_DEVICE_CONTROL == 0x0f, "internal major ABI");
_Static_assert(OwnedIrpInternalIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                                 METHOD_NEITHER,
                                                 FILE_ANY_ACCESS),
               "internal control code");

enum {
  InvalidAllocation = 1U << 0,
  InvalidDispatch = 1U << 1,
  InvalidCompletion = 1U << 2,
  InvalidWorker = 1U << 3,
  InvalidCancel = 1U << 4,
  InvalidRelease = 1U << 5,
  InvalidUnload = 1U << 6
};

typedef struct {
  ULONG Values[OwnedIrpSnapshotWords];
  UCHAR Mode;
  BOOLEAN CallerStack;
  PIRP Child;
  PIRP Outer;
  PIO_WORKITEM Work;
  ULONG *Buffer;
} OWNED_IRP_STATE;

static OWNED_IRP_STATE State;
static PDEVICE_OBJECT UpperDevice;
static PDEVICE_OBJECT LowerDevice;
static const WCHAR DeviceName[] = L"\\Device\\NeverDOwnedIrp";

static VOID Check(BOOLEAN Condition, ULONG Failure) {
  if (!Condition) {
    State.Values[OwnedIrpFailures] |= Failure;
    DbgPrint("WDM owned IRP: invalid contract %lu\n", Failure);
  }
}

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static BOOLEAN CompletesOuter(void) {
  return State.Mode == OwnedIrpWorker || State.Mode == OwnedIrpCancel ||
         State.Mode == OwnedIrpNested;
}

static NTSTATUS ChildCompletion(PDEVICE_OBJECT Device, PIRP Irp,
                                PVOID Context) {
  const BOOLEAN Pending =
      State.Mode == OwnedIrpWorker || State.Mode == OwnedIrpCancel;
  Check(Context == &State && Irp == State.Child &&
            Device == (State.CallerStack ? UpperDevice : NULL) &&
            KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Irp->PendingReturned == Pending && Irp->IoStatus.Information == 0,
        InvalidCompletion);
  ++State.Values[OwnedIrpCompletions];
  State.Values[OwnedIrpLastStatus] = Irp->IoStatus.Status;
  State.Values[OwnedIrpLastPending] = Irp->PendingReturned;
  State.Values[OwnedIrpLastDeviceIsUpper] = Device == UpperDevice;
  if (State.Mode == OwnedIrpHold) {
    State.Values[OwnedIrpHeld] = TRUE;
    return STATUS_MORE_PROCESSING_REQUIRED;
  }
  IoFreeIrp(Irp);
  ++State.Values[OwnedIrpFrees];
  State.Child = NULL;
  if (State.Mode == OwnedIrpKernelBuffer) {
    Check(State.Buffer && *State.Buffer == OwnedIrpBufferWritten,
          InvalidCompletion);
    *State.Buffer = OwnedIrpBufferInitial;
    ExFreePoolWithTag(State.Buffer, OwnedIrpBufferInitial);
    State.Buffer = NULL;
  }
  // Driver state remains live even though the packet was freed in this
  // callback.
  DbgPrint("WDM owned IRP: completion freed packet\n");
  if (CompletesOuter()) {
    PIRP Outer = State.Outer;
    State.Outer = NULL;
    ++State.Values[OwnedIrpNestedCompletions];
    Complete(Outer, STATUS_SUCCESS, 0);
    DbgPrint("WDM owned IRP: nested outer completion returned\n");
  }
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID CancelChild(PDEVICE_OBJECT Device, PIRP Irp) {
  const BOOLEAN Valid = Device == LowerDevice && State.Child == Irp &&
                        Irp->Cancel && !Irp->CancelRoutine &&
                        KeGetCurrentIrql() == DISPATCH_LEVEL;
  ++State.Values[OwnedIrpCancelCalls];
  IoReleaseCancelSpinLock(Irp->CancelIrql);
  Check(Valid && KeGetCurrentIrql() == PASSIVE_LEVEL, InvalidCancel);
  Complete(Irp, STATUS_CANCELLED, 0);
}

static VOID CompleteWorker(PDEVICE_OBJECT Device, PVOID Context) {
  PIRP Irp = State.Child;
  Check(Device == LowerDevice && Context == &State && Irp && State.Work &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidWorker);
  ++State.Values[OwnedIrpWorkers];
  IoFreeWorkItem(State.Work);
  State.Work = NULL;
  Complete(Irp, STATUS_SUCCESS, 0);
  DbgPrint("WDM owned IRP: worker returned after completion\n");
}

static NTSTATUS InternalDispatch(PDEVICE_OBJECT Device, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  Check(Device == LowerDevice && Irp == State.Child &&
            Stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL &&
            Stack->Parameters.DeviceIoControl.IoControlCode ==
                OwnedIrpInternalIoctl &&
            Irp->RequestorMode == KernelMode && !Irp->MdlAddress,
        InvalidDispatch);
  if (State.Mode == OwnedIrpKernelBuffer) {
    Check(Stack->Parameters.DeviceIoControl.InputBufferLength ==
                  sizeof(ULONG) &&
              Stack->Parameters.DeviceIoControl.OutputBufferLength ==
                  sizeof(ULONG) &&
              Stack->Parameters.DeviceIoControl.Type3InputBuffer ==
                  State.Buffer &&
              Irp->UserBuffer == State.Buffer && State.Buffer &&
              *State.Buffer == OwnedIrpBufferInitial,
          InvalidDispatch);
    *State.Buffer = OwnedIrpBufferWritten;
  } else {
    Check(!Stack->Parameters.DeviceIoControl.InputBufferLength &&
              !Stack->Parameters.DeviceIoControl.OutputBufferLength,
          InvalidDispatch);
  }
  ++State.Values[OwnedIrpLowerDispatches];
  if (State.Mode == OwnedIrpWorker) {
    State.Work = IoAllocateWorkItem(Device);
    if (!State.Work)
      return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(State.Work, CompleteWorker, DelayedWorkQueue, &State);
    return STATUS_PENDING;
  }
  if (State.Mode == OwnedIrpCancel) {
    KIRQL OldIrql;
    IoAcquireCancelSpinLock(&OldIrql);
    Check(!Irp->Cancel && IoSetCancelRoutine(Irp, CancelChild) == NULL,
          InvalidCancel);
    IoMarkIrpPending(Irp);
    IoReleaseCancelSpinLock(OldIrql);
    return STATUS_PENDING;
  }
  return Complete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS Submit(PIRP Outer, const UCHAR *Input) {
  const UCHAR Mode = Input[0];
  const CCHAR StackCount = LowerDevice->StackSize + (Input[1] ? 1 : 0);
  PIRP Child;
  PIO_STACK_LOCATION Stack;
  NTSTATUS Status;
  if (State.Child || Input[1] > TRUE ||
      (Mode != OwnedIrpInline && Mode != OwnedIrpWorker &&
       Mode != OwnedIrpHold && Mode != OwnedIrpCancel &&
       Mode != OwnedIrpNested && Mode != OwnedIrpUnsent &&
       Mode != OwnedIrpKernelBuffer))
    return Complete(Outer, STATUS_INVALID_PARAMETER, 0);
  Child = IoAllocateIrp(StackCount, FALSE);
  if (!Child)
    return Complete(Outer, STATUS_INSUFFICIENT_RESOURCES, 0);
  ++State.Values[OwnedIrpAllocations];
  Check(Child->Type == IO_TYPE_IRP && Child->Size == IoSizeOfIrp(StackCount) &&
            Child->StackCount == StackCount &&
            Child->CurrentLocation == StackCount + 1 &&
            IoGetNextIrpStackLocation(Child) ==
                (PIO_STACK_LOCATION)(Child + 1) + StackCount - 1,
        InvalidAllocation);
  if (Mode == OwnedIrpUnsent) {
    IoFreeIrp(Child);
    ++State.Values[OwnedIrpFrees];
    return Complete(Outer, STATUS_SUCCESS, 0);
  }
  State.Mode = Mode;
  State.CallerStack = Input[1];
  State.Child = Child;
  State.Outer = CompletesOuter() ? Outer : NULL;
  if (State.CallerStack) {
    IoSetNextIrpStackLocation(Child);
    IoGetCurrentIrpStackLocation(Child)->DeviceObject = UpperDevice;
  }
  Stack = IoGetNextIrpStackLocation(Child);
  Stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
  Stack->Parameters.DeviceIoControl.IoControlCode = OwnedIrpInternalIoctl;
  if (Mode == OwnedIrpKernelBuffer) {
    State.Buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ULONG),
                                   OwnedIrpBufferInitial);
    if (!State.Buffer) {
      IoFreeIrp(Child);
      State.Child = NULL;
      ++State.Values[OwnedIrpFrees];
      return Complete(Outer, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    *State.Buffer = OwnedIrpBufferInitial;
    Stack->Parameters.DeviceIoControl.InputBufferLength = sizeof(ULONG);
    Stack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(ULONG);
    Stack->Parameters.DeviceIoControl.Type3InputBuffer = State.Buffer;
    Child->UserBuffer = State.Buffer;
  }
  Child->RequestorMode = KernelMode;
  IoSetCompletionRoutine(Child, ChildCompletion, &State, TRUE, TRUE, TRUE);
  if (CompletesOuter())
    IoMarkIrpPending(Outer);
  Status = IoCallDriver(LowerDevice, Child);
  ++State.Values[OwnedIrpDispatchReturns];
  State.Values[OwnedIrpLastDispatchStatus] = Status;
  if (Mode == OwnedIrpCancel)
    Check(IoCancelIrp(Child), InvalidCancel);
  // Both packets may already be gone. A driver-owned snapshot is authoritative.
  if (CompletesOuter())
    return STATUS_PENDING;
  return Complete(Outer, STATUS_SUCCESS, 0);
}

static NTSTATUS Dispatch(PDEVICE_OBJECT Device, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Complete(Irp, STATUS_SUCCESS, 0);
  if (Device != UpperDevice)
    return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
  const ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
  if (Code == OwnedIrpSubmitIoctl) {
    if (Stack->Parameters.DeviceIoControl.InputBufferLength < 2)
      return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    return Submit(Irp, Irp->AssociatedIrp.SystemBuffer);
  }
  if (Code == OwnedIrpReleaseIoctl) {
    if (!State.Child || !State.Values[OwnedIrpHeld])
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    Check(State.Child->IoStatus.Status == STATUS_SUCCESS &&
              State.Child->IoStatus.Information == 0 &&
              !State.Child->PendingReturned,
          InvalidRelease);
    IoFreeIrp(State.Child);
    State.Child = NULL;
    State.Values[OwnedIrpHeld] = FALSE;
    ++State.Values[OwnedIrpFrees];
    return Complete(Irp, STATUS_SUCCESS, 0);
  }
  if (Code == OwnedIrpSnapshotIoctl) {
    if (Stack->Parameters.DeviceIoControl.OutputBufferLength <
        sizeof(State.Values))
      return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, State.Values,
                  sizeof(State.Values));
    return Complete(Irp,
                    State.Values[OwnedIrpFailures] ? STATUS_INVALID_DEVICE_STATE
                                                   : STATUS_SUCCESS,
                    sizeof(State.Values));
  }
  return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static VOID DriverUnload(PDRIVER_OBJECT Driver) {
  UNREFERENCED_PARAMETER(Driver);
  Check(!State.Child && !State.Outer && !State.Work && !State.Buffer &&
            State.Values[OwnedIrpAllocations] == State.Values[OwnedIrpFrees],
        InvalidUnload);
  DbgPrint("WDM owned IRP: unload failures %lu allocations %lu frees %lu\n",
           State.Values[OwnedIrpFailures], State.Values[OwnedIrpAllocations],
           State.Values[OwnedIrpFrees]);
  if (State.Values[OwnedIrpFailures])
    ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
  IoDeleteDevice(LowerDevice);
  IoDeleteDevice(UpperDevice);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  UNICODE_STRING Name;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(RegistryPath);
  for (ULONG I = 0; I <= IRP_MJ_MAXIMUM_FUNCTION; ++I)
    Driver->MajorFunction[I] = Dispatch;
  Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = InternalDispatch;
  Driver->DriverUnload = DriverUnload;
  RtlInitUnicodeString(&Name, DeviceName);
  Status = IoCreateDevice(Driver, 0, &Name, FILE_DEVICE_UNKNOWN, 0, FALSE,
                          &UpperDevice);
  if (!NT_SUCCESS(Status))
    return Status;
  Status = IoCreateDevice(Driver, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE,
                          &LowerDevice);
  if (!NT_SUCCESS(Status)) {
    IoDeleteDevice(UpperDevice);
    return Status;
  }
  UpperDevice->Flags |= DO_BUFFERED_IO;
  UpperDevice->Flags &= ~DO_DEVICE_INITIALIZING;
  LowerDevice->Flags &= ~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
}

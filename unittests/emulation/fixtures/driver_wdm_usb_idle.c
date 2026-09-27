//===- driver_wdm_usb_idle.c - Genuine WDK USB idle lifetime -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Submit a caller-owned USB idle packet to the actual lower stack. The bus
/// permission callback waits for its D2 transaction; the idle packet remains
/// independent of both that transaction and a later D0 request.
///
//===----------------------------------------------------------------------===//
#include "driver_wdm_usb_idle_test.h"

#include <ntddk.h>
#include <usbioctl.h>

#define ABI_OFFSET(Type, Member, Offset)                                       \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset, #Type "." #Member)
_Static_assert(sizeof(IRP) == 0xd0 && sizeof(IO_STACK_LOCATION) == 0x48,
               "x64 WDM packet ABI");
_Static_assert(sizeof(USB_IDLE_CALLBACK_INFO) == 16, "USB idle input ABI");
ABI_OFFSET(USB_IDLE_CALLBACK_INFO, IdleCallback, 0);
ABI_OFFSET(USB_IDLE_CALLBACK_INFO, IdleContext, 8);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.DeviceIoControl.Type3InputBuffer, 32);
_Static_assert(IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION ==
                   UsbIdleInternalIoctl,
               "USB idle control code");
_Static_assert((IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION & 3) ==
                       METHOD_NEITHER &&
                   IRP_MJ_INTERNAL_DEVICE_CONTROL == 0x0f,
               "kernel internal METHOD_NEITHER request");
_Static_assert(PowerDeviceD0 == 1 && PowerDeviceD2 == 3 && PowerDeviceD3 == 4,
               "WDM device power states");
_Static_assert(UsbIdleCancelledStatus == (ULONG)STATUS_CANCELLED &&
                   UsbIdleBusyStatus == (ULONG)STATUS_DEVICE_BUSY &&
                   UsbIdlePowerInvalidStatus ==
                       (ULONG)STATUS_POWER_STATE_INVALID,
               "idle terminal status observations");
_Static_assert(UsbIdleCommandIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                               METHOD_BUFFERED,
                                               FILE_ANY_ACCESS),
               "command control code");
_Static_assert(UsbIdleSnapshotIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801,
                                                METHOD_BUFFERED,
                                                FILE_ANY_ACCESS),
               "snapshot control code");
typedef VOID (*IDLE_CALLBACK_ABI)(PVOID);
_Static_assert(__builtin_types_compatible_p(USB_IDLE_CALLBACK,
                                            IDLE_CALLBACK_ABI),
               "one-argument void idle callback ABI");

enum {
  InvalidAllocation = 1U << 0,
  InvalidSubmission = 1U << 1,
  InvalidIdleCallback = 1U << 2,
  InvalidIdleCompletion = 1U << 3,
  InvalidPower = 1U << 4,
  InvalidCancel = 1U << 5,
  InvalidWake = 1U << 6,
  InvalidPnp = 1U << 7,
  InvalidUnload = 1U << 8,
  IdlePoolTag = 0x49627355,
  PacketCount = 2
};

typedef struct USB_EXTENSION USB_EXTENSION;
typedef struct {
  USB_EXTENSION *Owner;
  PIRP Irp;
  PUSB_IDLE_CALLBACK_INFO Info;
  UCHAR Mode;
  BOOLEAN Entered;
  BOOLEAN Returned;
} IDLE_PACKET;

struct USB_EXTENSION {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  IDLE_PACKET Packets[PacketCount];
  PIRP WakeIRP;
  PIO_WORKITEM CancelWork;
  KEVENT PnpDone;
  KEVENT PowerDone;
  DEVICE_POWER_STATE RequestedPower;
  DEVICE_POWER_STATE AcknowledgedPower;
  BOOLEAN PowerPending;
  ULONG Sequence;
  ULONG Values[UsbIdleSnapshotWords];
};

static BOOLEAN CallerStack;
static ULONG LiveDevices;
static ULONG Failures;

static VOID Check(USB_EXTENSION *Extension, BOOLEAN Condition, ULONG Failure) {
  if (!Condition) {
    Extension->Values[UsbIdleFailures] |= Failure;
    Failures |= Failure;
    DbgPrint("WDM USB idle: invalid contract %lu\n", Failure);
  }
}

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static VOID PowerComplete(PDEVICE_OBJECT Device, UCHAR Minor, POWER_STATE State,
                          PVOID Context, PIO_STATUS_BLOCK IoStatus) {
  USB_EXTENSION *Extension = Context;
  Check(Extension,
        Device == Extension->Self && Minor == IRP_MN_SET_POWER &&
            Extension->PowerPending &&
            State.DeviceState == Extension->RequestedPower &&
            KeGetCurrentIrql() == PASSIVE_LEVEL &&
            IoStatus->Status == STATUS_SUCCESS && !IoStatus->Information &&
            Extension->AcknowledgedPower == State.DeviceState,
        InvalidPower);
  if (State.DeviceState == PowerDeviceD2) {
    ++Extension->Values[UsbIdleD2Completions];
    Extension->Values[UsbIdleD2CompletionOrder] = ++Extension->Sequence;
  } else if (State.DeviceState == PowerDeviceD0) {
    ++Extension->Values[UsbIdleD0Completions];
    Extension->Values[UsbIdleD0CompletionOrder] = ++Extension->Sequence;
  } else if (State.DeviceState == PowerDeviceD3) {
    ++Extension->Values[UsbIdleD3Completions];
  }
  Extension->PowerPending = FALSE;
  KeSetEvent(&Extension->PowerDone, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS RequestPower(USB_EXTENSION *Extension,
                             DEVICE_POWER_STATE DeviceState) {
  Check(Extension, !Extension->PowerPending, InvalidPower);
  Extension->PowerPending = TRUE;
  Extension->RequestedPower = DeviceState;
  KeClearEvent(&Extension->PowerDone);
  POWER_STATE State;
  State.DeviceState = DeviceState;
  const NTSTATUS Status = PoRequestPowerIrp(
      Extension->Self, IRP_MN_SET_POWER, State, PowerComplete, Extension, NULL);
  Check(Extension, Status == STATUS_PENDING, InvalidPower);
  return Status;
}

static VOID WaitForPower(USB_EXTENSION *Extension) {
  Check(Extension,
        KeWaitForSingleObject(&Extension->PowerDone, Executive, KernelMode,
                              FALSE, NULL) == STATUS_SUCCESS &&
            !Extension->PowerPending,
        InvalidPower);
}

static VOID WakeComplete(PDEVICE_OBJECT Device, UCHAR Minor, POWER_STATE State,
                         PVOID Context, PIO_STATUS_BLOCK IoStatus) {
  USB_EXTENSION *Extension = Context;
  const NTSTATUS Status = IoStatus->Status;
  Check(Extension,
        Device == Extension->Self && Minor == IRP_MN_WAIT_WAKE &&
            State.SystemState == PowerSystemWorking && Extension->WakeIRP &&
            !IoStatus->Information,
        InvalidWake);
  Extension->WakeIRP = NULL;
  ++Extension->Values[UsbIdleWakeCallbacks];
  Extension->Values[UsbIdleLastWakeStatus] = Status;
  if (Status == STATUS_SUCCESS)
    RequestPower(Extension, PowerDeviceD0);
  Check(Extension, IoStatus->Status == Status && !IoStatus->Information,
        InvalidWake);
}

static VOID ArmWake(USB_EXTENSION *Extension) {
  Check(Extension, !Extension->WakeIRP && KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidWake);
  POWER_STATE State;
  State.SystemState = PowerSystemWorking;
  ++Extension->Values[UsbIdleWakeSubmissions];
  Check(Extension,
        PoRequestPowerIrp(Extension->Self, IRP_MN_WAIT_WAKE, State,
                          WakeComplete, Extension,
                          &Extension->WakeIRP) == STATUS_PENDING,
        InvalidWake);
}

static VOID CancelIdle(IDLE_PACKET *Packet) {
  USB_EXTENSION *Extension = Packet->Owner;
  if (!Packet->Irp)
    return;
  const BOOLEAN InCallback = Packet->Entered && !Packet->Returned;
  ++Extension->Values[UsbIdleCancelCalls];
  const BOOLEAN Cancelled = IoCancelIrp(Packet->Irp);
  Extension->Values[UsbIdleCancelTrue] += Cancelled != FALSE;
  Check(Extension,
        Cancelled && (InCallback ? Packet->Irp != NULL : Packet->Irp == NULL),
        InvalidCancel);
  if (InCallback) {
    Check(Extension, Packet->Info && !Packet->Irp->CancelRoutine,
          InvalidCancel);
    ++Extension->Values[UsbIdleRetainedAfterInCallbackCancel];
  }
}

static VOID CancelWorker(PDEVICE_OBJECT Device, PVOID Context) {
  IDLE_PACKET *Packet = Context;
  USB_EXTENSION *Extension = Packet->Owner;
  Check(Extension,
        Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Extension->CancelWork && Packet->Entered && !Packet->Returned &&
            Extension->PowerPending &&
            Extension->RequestedPower == PowerDeviceD2,
        InvalidCancel);
  ++Extension->Values[UsbIdleCancelWorkers];
  PIO_WORKITEM Work = Extension->CancelWork;
  Extension->CancelWork = NULL;
  IoFreeWorkItem(Work);
  CancelIdle(Packet);
}

static VOID IdleCallback(PVOID Context) {
  IDLE_PACKET *Packet = Context;
  USB_EXTENSION *Extension = Packet->Owner;
  Check(Extension,
        KeGetCurrentIrql() == PASSIVE_LEVEL && Packet->Irp && Packet->Info &&
            Packet->Info->IdleContext == Packet && !Packet->Entered &&
            Extension->AcknowledgedPower == PowerDeviceD0 &&
            Extension->Values[UsbIdleSystemPower] == PowerSystemWorking,
        InvalidIdleCallback);
  Packet->Entered = TRUE;
  ++Extension->Values[UsbIdleCallbackEntries];
  Extension->Values[UsbIdleEntryOrder] = ++Extension->Sequence;
  if (Packet->Mode == UsbIdleSubmitWithRemoteWake)
    ArmWake(Extension);
  if (Packet->Mode == UsbIdleSubmitWithCancelWorker) {
    Extension->CancelWork = IoAllocateWorkItem(Extension->Self);
    Check(Extension, Extension->CancelWork != NULL, InvalidAllocation);
    if (Extension->CancelWork)
      IoQueueWorkItem(Extension->CancelWork, CancelWorker, DelayedWorkQueue,
                      Packet);
  }
  if (RequestPower(Extension, PowerDeviceD2) == STATUS_PENDING)
    WaitForPower(Extension);
  Check(Extension,
        Packet->Irp && Packet->Info && !Extension->PowerPending &&
            Extension->AcknowledgedPower == PowerDeviceD2 &&
            Extension->Values[UsbIdleD2CompletionOrder] >
                Extension->Values[UsbIdleEntryOrder] &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidIdleCallback);
  ++Extension->Values[UsbIdleCallbackReturns];
  Extension->Values[UsbIdleReturnOrder] = ++Extension->Sequence;
  Packet->Returned = TRUE;
}

static NTSTATUS IdleComplete(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  IDLE_PACKET *Packet = Context;
  USB_EXTENSION *Extension = Packet->Owner;
  Check(Extension,
        Packet->Irp == Irp && Packet->Info &&
            Device == (CallerStack ? Extension->Self : NULL) &&
            (!Packet->Entered || Packet->Returned) &&
            !Irp->IoStatus.Information,
        InvalidIdleCompletion);
  ++Extension->Values[UsbIdleCompletions];
  Extension->Values[UsbIdleLastStatus] = Irp->IoStatus.Status;
  Extension->Values[UsbIdleCompletionOrder] = ++Extension->Sequence;
  Extension->Values[UsbIdleLastCompletionIrql] = KeGetCurrentIrql();
  Extension->Values[UsbIdleLastCompletionDeviceIsSelf] =
      Device == Extension->Self;
  if (Irp->IoStatus.Status == STATUS_SUCCESS && Extension->PowerPending &&
      Extension->RequestedPower == PowerDeviceD0) {
    Check(Extension,
          Extension->Values[UsbIdleD0DispatchOrder] <
                  Extension->Values[UsbIdleCompletionOrder] &&
              Extension->Values[UsbIdleD0CompletionOrder] <
                  Extension->Values[UsbIdleD0DispatchOrder],
          InvalidIdleCompletion);
    ++Extension->Values[UsbIdleCompletedBeforeD0Acknowledgement];
  }
  PUSB_IDLE_CALLBACK_INFO Info = Packet->Info;
  Packet->Info = NULL;
  Packet->Irp = NULL;
  ExFreePoolWithTag(Info, IdlePoolTag);
  IoFreeIrp(Irp);
  ++Extension->Values[UsbIdleFrees];
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SubmitIdle(USB_EXTENSION *Extension, UCHAR Mode) {
  IDLE_PACKET *Packet = &Extension->Packets[Mode == UsbIdleSubmitDuplicate];
  if (Packet->Irp || Extension->AcknowledgedPower != PowerDeviceD0)
    return STATUS_INVALID_DEVICE_STATE;
  PDEVICE_OBJECT Target =
      Mode == UsbIdleSubmitThroughStack ? Extension->Self : Extension->Lower;
  const CCHAR StackCount = Target->StackSize + CallerStack;
  PUSB_IDLE_CALLBACK_INFO Info =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*Info), IdlePoolTag);
  if (!Info)
    return STATUS_INSUFFICIENT_RESOURCES;
  PIRP Irp = IoAllocateIrp(StackCount, FALSE);
  if (!Irp) {
    ExFreePoolWithTag(Info, IdlePoolTag);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  ++Extension->Values[UsbIdleAllocations];
  Packet->Owner = Extension;
  Packet->Irp = Irp;
  Packet->Info = Info;
  Packet->Mode = Mode;
  Packet->Entered = FALSE;
  Packet->Returned = FALSE;
  Info->IdleCallback = IdleCallback;
  Info->IdleContext = Packet;
  if (CallerStack) {
    IoSetNextIrpStackLocation(Irp);
    IoGetCurrentIrpStackLocation(Irp)->DeviceObject = Extension->Self;
  }
  PIO_STACK_LOCATION Stack = IoGetNextIrpStackLocation(Irp);
  Stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
  Stack->Parameters.DeviceIoControl.IoControlCode =
      IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION;
  Stack->Parameters.DeviceIoControl.InputBufferLength = sizeof(*Info);
  Stack->Parameters.DeviceIoControl.Type3InputBuffer = Info;
  Irp->RequestorMode = KernelMode;
  IoSetCompletionRoutine(Irp, IdleComplete, Packet, TRUE, TRUE, TRUE);
  const NTSTATUS Status = IoCallDriver(Target, Irp);
  ++Extension->Values[UsbIdleDispatchReturns];
  Extension->Values[UsbIdleLastDispatchStatus] = Status;
  Check(Extension,
        Mode == UsbIdleSubmitDuplicate ? Status == STATUS_DEVICE_BUSY
                                       : Status == STATUS_PENDING,
        InvalidSubmission);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchInternal(PDEVICE_OBJECT Device, PIRP Irp) {
  USB_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  Check(Extension,
        Stack->Parameters.DeviceIoControl.IoControlCode ==
                IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION &&
            Irp == Extension->Packets[0].Irp,
        InvalidSubmission);
  ++Extension->Values[UsbIdleInternalDispatches];
  IoSkipCurrentIrpStackLocation(Irp);
  return IoCallDriver(Extension->Lower, Irp);
}

static NTSTATUS PowerCompletion(PDEVICE_OBJECT Device, PIRP Irp,
                                PVOID Context) {
  USB_EXTENSION *Extension = Context;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  Check(Extension, Device == Extension->Self, InvalidPower);
  if (Stack->MinorFunction == IRP_MN_SET_POWER &&
      Irp->IoStatus.Status == STATUS_SUCCESS) {
    if (Stack->Parameters.Power.Type == DevicePowerState) {
      Extension->AcknowledgedPower = Stack->Parameters.Power.State.DeviceState;
      Extension->Values[UsbIdleDevicePower] = Extension->AcknowledgedPower;
    } else {
      Extension->Values[UsbIdleSystemPower] =
          Stack->Parameters.Power.State.SystemState;
    }
  }
  if (Irp->PendingReturned)
    IoMarkIrpPending(Irp);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Device, PIRP Irp) {
  USB_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  if (Stack->MinorFunction == IRP_MN_SET_POWER &&
      Stack->Parameters.Power.Type == DevicePowerState &&
      Stack->Parameters.Power.State.DeviceState == PowerDeviceD0)
    Extension->Values[UsbIdleD0DispatchOrder] = ++Extension->Sequence;
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, PowerCompletion, Extension, TRUE, TRUE, TRUE);
  return PoCallDriver(Extension->Lower, Irp);
}

static NTSTATUS PnpCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  USB_EXTENSION *Extension = Context;
  Check(Extension,
        Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            !Irp->IoStatus.Information,
        InvalidPnp);
  KeSetEvent(&Extension->PnpDone, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  USB_EXTENSION *Extension = Device->DeviceExtension;
  const UCHAR Minor = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
  if (Minor == IRP_MN_QUERY_STOP_DEVICE || Minor == IRP_MN_STOP_DEVICE ||
      Minor == IRP_MN_QUERY_REMOVE_DEVICE || Minor == IRP_MN_REMOVE_DEVICE ||
      Minor == IRP_MN_SURPRISE_REMOVAL) {
    for (unsigned I = 0; I < PacketCount; ++I)
      CancelIdle(&Extension->Packets[I]);
    if (Extension->WakeIRP)
      Check(Extension, IoCancelIrp(Extension->WakeIRP), InvalidWake);
  }
  if (Minor == IRP_MN_STOP_DEVICE)
    ++Extension->Values[UsbIdleStops];
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    Check(Extension,
          !Extension->Packets[0].Irp && !Extension->Packets[1].Irp &&
              !Extension->WakeIRP && !Extension->PowerPending &&
              !Extension->CancelWork,
          InvalidPnp);
    IoSkipCurrentIrpStackLocation(Irp);
    const NTSTATUS Status = IoCallDriver(Lower, Irp);
    IoDetachDevice(Lower);
    --LiveDevices;
    IoDeleteDevice(Device);
    return Status;
  }
  KeClearEvent(&Extension->PnpDone);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, PnpCompletion, Extension, TRUE, TRUE, TRUE);
  NTSTATUS Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    Check(Extension,
          KeWaitForSingleObject(&Extension->PnpDone, Executive, KernelMode,
                                FALSE, NULL) == STATUS_SUCCESS,
          InvalidPnp);
  Status = Irp->IoStatus.Status;
  if (Minor == IRP_MN_START_DEVICE && NT_SUCCESS(Status)) {
    ++Extension->Values[UsbIdleStarts];
    Extension->AcknowledgedPower = PowerDeviceD0;
    Extension->Values[UsbIdleDevicePower] = PowerDeviceD0;
    Extension->Values[UsbIdleSystemPower] = PowerSystemWorking;
  }
  return Complete(Irp, Status, 0);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  USB_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Complete(Irp, STATUS_SUCCESS, 0);
  const ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
  if (Code == UsbIdleSnapshotIoctl &&
      Stack->Parameters.DeviceIoControl.OutputBufferLength >=
          sizeof(Extension->Values)) {
    const PIRP Idle = Extension->Packets[0].Irp;
    Extension->Values[UsbIdleActive] = Idle != NULL;
    Extension->Values[UsbIdleHasCancelRoutine] = Idle && Idle->CancelRoutine;
    Extension->Values[UsbIdleWakeActive] = Extension->WakeIRP != NULL;
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Extension->Values,
                  sizeof(Extension->Values));
    return Complete(Irp, STATUS_SUCCESS, sizeof(Extension->Values));
  }
  if (Code != UsbIdleCommandIoctl ||
      Stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(UCHAR))
    return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
  const UCHAR Command = *(UCHAR *)Irp->AssociatedIrp.SystemBuffer;
  NTSTATUS Status = STATUS_SUCCESS;
  switch (Command) {
  case UsbIdleSubmit:
  case UsbIdleSubmitWithCancelWorker:
  case UsbIdleSubmitWithRemoteWake:
  case UsbIdleSubmitThroughStack:
  case UsbIdleSubmitDuplicate:
    Status = SubmitIdle(Extension, Command);
    break;
  case UsbIdleCancel:
    CancelIdle(&Extension->Packets[0]);
    break;
  case UsbIdleCancelRearm:
    CancelIdle(&Extension->Packets[0]);
    Status = SubmitIdle(Extension, UsbIdleSubmit);
    break;
  case UsbIdleRequestD0:
  case UsbIdleRequestD3:
    if (RequestPower(Extension, Command == UsbIdleRequestD0
                                    ? PowerDeviceD0
                                    : PowerDeviceD3) == STATUS_PENDING)
      WaitForPower(Extension);
    break;
  default:
    Status = STATUS_INVALID_PARAMETER;
    break;
  }
  return Complete(Irp, Status, 0);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status =
      IoCreateDevice(Driver, sizeof(USB_EXTENSION), NULL, FILE_DEVICE_UNKNOWN,
                     FILE_DEVICE_SECURE_OPEN, FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  USB_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  for (unsigned I = 0; I < PacketCount; ++I)
    Extension->Packets[I].Owner = Extension;
  KeInitializeEvent(&Extension->PnpDone, NotificationEvent, FALSE);
  KeInitializeEvent(&Extension->PowerDone, NotificationEvent, FALSE);
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  ++LiveDevices;
  return STATUS_SUCCESS;
}

static VOID Unload(PDRIVER_OBJECT Driver) {
  if (LiveDevices || Driver->DeviceObject)
    Failures |= InvalidUnload;
  DbgPrint("WDM USB idle: unload live %lu failures %lu\n", LiveDevices,
           Failures);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  if (RegistryPath->Length >= sizeof(WCHAR))
    CallerStack =
        RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1] ==
        UsbIdleCallerStackMode;
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
  Driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternal;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  return STATUS_SUCCESS;
}

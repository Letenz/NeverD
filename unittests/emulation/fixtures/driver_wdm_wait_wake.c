//===- driver_wdm_wait_wake.c - Genuine WDK native WAIT_WAKE ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retain a power-manager-owned WAIT_WAKE through the actual WDM stack.
/// Completion, cancellation and subsequent power requests keep distinct IRPs.
///
//===----------------------------------------------------------------------===//
#include "driver_wdm_wait_wake_test.h"

#include <ntddk.h>

#define ABI_OFFSET(Type, Member, Value)                                        \
  _Static_assert(__builtin_offsetof(Type, Member) == Value, #Type "." #Member)
_Static_assert(sizeof(IRP) == 0xd0 && sizeof(IO_STACK_LOCATION) == 0x48,
               "x64 WDM packet ABI");
_Static_assert(sizeof(POWER_STATE) == 4 && sizeof(IO_STATUS_BLOCK) == 16,
               "power request and terminal status ABI");
ABI_OFFSET(IO_STACK_LOCATION, Parameters.WaitWake.PowerState, 8);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.Power.Type, 16);
ABI_OFFSET(IO_STACK_LOCATION, Parameters.Power.State, 24);
ABI_OFFSET(IRP, CancelRoutine, 0x68);
_Static_assert(IRP_MN_WAIT_WAKE == 0 && PowerSystemWorking == 1 &&
                   PowerSystemSleeping3 == 4 && DISPATCH_LEVEL == 2,
               "native wake state and IRQL codes");
_Static_assert(WdmWakeCancelledStatus == (ULONG)STATUS_CANCELLED,
               "cancel status observation");
_Static_assert(WdmWakeSnapshotIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                                METHOD_BUFFERED,
                                                FILE_ANY_ACCESS),
               "snapshot control code");
_Static_assert(WdmWakeCommandIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801,
                                               METHOD_BUFFERED,
                                               FILE_ANY_ACCESS),
               "command control code");
_Static_assert(WdmWakeExtendedSnapshotIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN,
                                                        0x802, METHOD_BUFFERED,
                                                        FILE_ANY_ACCESS),
               "extended snapshot control code");
typedef VOID (*WAKE_CALLBACK_ABI)(PDEVICE_OBJECT, UCHAR, POWER_STATE, PVOID,
                                  PIO_STATUS_BLOCK);
_Static_assert(__builtin_types_compatible_p(PREQUEST_POWER_COMPLETE,
                                            WAKE_CALLBACK_ABI),
               "five-argument void power completion ABI");

enum {
  InvalidSend = 1U << 0,
  InvalidDispatch = 1U << 1,
  InvalidCompletion = 1U << 2,
  InvalidCallback = 1U << 3,
  InvalidCancellation = 1U << 4,
  InvalidWorker = 1U << 5,
  InvalidPnp = 1U << 6,
  InvalidUnload = 1U << 7,
  InvalidQueuedPower = 1U << 8
};

typedef struct WAKE_EXTENSION WAKE_EXTENSION;
typedef struct {
  WAKE_EXTENSION *Owner;
  PIRP Irp;
  UCHAR Minor;
  DEVICE_POWER_STATE State;
  BOOLEAN Pending;
} QUEUED_POWER_CONTEXT;

struct WAKE_EXTENSION {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  PIRP WakeIRP;
  PIRP Outer;
  PIO_WORKITEM Work;
  KEVENT PnpDone;
  KEVENT PowerDone;
  KDPC CancelDpc;
  ULONG Values[WdmWakeExtendedSnapshotWords];
  QUEUED_POWER_CONTEXT PowerContext;
  ULONG Sequence;
  UCHAR DpcCommand;
  UCHAR AfterWake;
};

static UCHAR Mode;
static ULONG Failures;
static ULONG LiveDevices;

static KIRQL ReadGuestCR8(VOID) {
  ULONG64 Value;
  __asm__ volatile("movq %%cr8, %0" : "=r"(Value));
  return (KIRQL)Value;
}

static VOID Check(WAKE_EXTENSION *Extension, BOOLEAN Condition, ULONG Failure) {
  if (!Condition) {
    Extension->Values[WdmWakeFailures] |= Failure;
    Failures |= Failure;
    DbgPrint("WDM wait wake: invalid contract %lu\n", Failure);
  }
}

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = Information;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static SYSTEM_POWER_STATE WakeLimit(VOID) {
  return Mode == WdmWakeModeSleeping ? PowerSystemSleeping3
                                     : PowerSystemWorking;
}

static PDEVICE_OBJECT RequestDevice(WAKE_EXTENSION *Extension) {
  return Mode == WdmWakeModePDO ? Extension->PDO : Extension->Self;
}

static REQUEST_POWER_COMPLETE WakeComplete;
static NTSTATUS ArmWake(WAKE_EXTENSION *Extension) {
  POWER_STATE State;
  State.SystemState = WakeLimit();
  Check(Extension, KeGetCurrentIrql() == PASSIVE_LEVEL && !Extension->WakeIRP,
        InvalidSend);
  ++Extension->Values[WdmWakeSubmissions];
  NTSTATUS Status = PoRequestPowerIrp(
      RequestDevice(Extension), IRP_MN_WAIT_WAKE, State,
      Mode == WdmWakeModeNoCallback ? NULL : WakeComplete, Extension,
      Mode == WdmWakeModeNoOutput ? NULL : &Extension->WakeIRP);
  Check(Extension, Status == STATUS_PENDING, InvalidSend);
  return Status;
}

static VOID DeviceComplete(PDEVICE_OBJECT Device, UCHAR Minor,
                           POWER_STATE State, PVOID Context,
                           PIO_STATUS_BLOCK IoStatus) {
  WAKE_EXTENSION *Extension = Context;
  Check(Extension,
        Device == Extension->Self && Minor == IRP_MN_SET_POWER &&
            State.DeviceState == PowerDeviceD0 &&
            KeGetCurrentIrql() == PASSIVE_LEVEL &&
            IoStatus->Status == STATUS_SUCCESS && !IoStatus->Information,
        InvalidCallback);
  ++Extension->Values[WdmWakeD0Callbacks];
  KeSetEvent(&Extension->PowerDone, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS RequestD0(WAKE_EXTENSION *Extension) {
  POWER_STATE State;
  State.DeviceState = PowerDeviceD0;
  KeClearEvent(&Extension->PowerDone);
  const NTSTATUS Status =
      PoRequestPowerIrp(Extension->Self, IRP_MN_SET_POWER, State,
                        DeviceComplete, Extension, NULL);
  Check(Extension, Status == STATUS_PENDING, InvalidSend);
  return Status;
}

static VOID QueuedPowerComplete(PDEVICE_OBJECT Device, UCHAR Minor,
                                POWER_STATE State, PVOID Context,
                                PIO_STATUS_BLOCK IoStatus) {
  QUEUED_POWER_CONTEXT *Power = Context;
  WAKE_EXTENSION *Extension = Power->Owner;
  Extension->Values[WdmWakePowerCallbackIrql] = KeGetCurrentIrql();
  Extension->Values[WdmWakePowerCallbackCR8] = ReadGuestCR8();
  Extension->Values[WdmWakePowerCallbackOrder] = ++Extension->Sequence;
  Check(Extension,
        Power == &Extension->PowerContext && Power->Pending &&
            Device == Extension->Self && Minor == Power->Minor &&
            State.DeviceState == Power->State &&
            KeGetCurrentIrql() == PASSIVE_LEVEL &&
            ReadGuestCR8() == PASSIVE_LEVEL &&
            IoStatus->Status == STATUS_SUCCESS && !IoStatus->Information &&
            Extension->Values[WdmWakePowerDispatchOrder] >
                Extension->Values[WdmWakeCallerReturnOrder] &&
            Extension->Values[WdmWakePowerCallbackOrder] >
                Extension->Values[WdmWakePowerDispatchOrder],
        InvalidQueuedPower);
  Power->Pending = FALSE;
  Power->Irp = NULL;
  ++Extension->Values[WdmWakeQueuedCompletions];
  if (Minor == IRP_MN_SET_POWER && State.DeviceState == PowerDeviceD0)
    ++Extension->Values[WdmWakeD0Callbacks];
}

static VOID RequestQueuedPower(WAKE_EXTENSION *Extension, UCHAR Minor,
                               DEVICE_POWER_STATE DeviceState) {
  QUEUED_POWER_CONTEXT *Context = &Extension->PowerContext;
  const KIRQL Irql = KeGetCurrentIrql();
  Check(Extension,
        !Context->Pending && (Irql == APC_LEVEL || Irql == DISPATCH_LEVEL) &&
            ReadGuestCR8() == Irql,
        InvalidQueuedPower);
  Context->Owner = Extension;
  Context->Minor = Minor;
  Context->State = DeviceState;
  Context->Pending = TRUE;
  Context->Irp = NULL;
  Extension->Values[WdmWakeCallerReturnOrder] = 0;
  Extension->Values[WdmWakePowerDispatchOrder] = 0;
  Extension->Values[WdmWakePowerCallbackOrder] = 0;
  Extension->Values[WdmWakeQueuedMinor] = Minor;
  Extension->Values[WdmWakeQueuedDeviceState] = DeviceState;
  Extension->Values[WdmWakeCallerIrqlBefore] = Irql;
  Extension->Values[WdmWakeCallerCR8Before] = ReadGuestCR8();
  ++Extension->Values[WdmWakeQueuedSubmissions];
  POWER_STATE State;
  State.DeviceState = DeviceState;
  const NTSTATUS Status = PoRequestPowerIrp(Extension->Self, Minor, State,
                                            QueuedPowerComplete, Context, NULL);
  Extension->Values[WdmWakeQueuedAPIStatus] = Status;
  Extension->Values[WdmWakeCallerIrqlAfter] = KeGetCurrentIrql();
  Extension->Values[WdmWakeCallerCR8After] = ReadGuestCR8();
  Check(Extension,
        Status == STATUS_PENDING && !Context->Irp && Context->Pending &&
            KeGetCurrentIrql() == Irql && ReadGuestCR8() == Irql &&
            !Extension->Values[WdmWakePowerDispatchOrder] &&
            !Extension->Values[WdmWakePowerCallbackOrder],
        InvalidQueuedPower);
}

static VOID PowerWorker(PDEVICE_OBJECT Device, PVOID Context) {
  WAKE_EXTENSION *Extension = Context;
  PIO_WORKITEM Work = Extension->Work;
  PIRP Outer = Extension->Outer;
  Check(Extension,
        Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            !Extension->WakeIRP && Work && Outer,
        InvalidWorker);
  ++Extension->Values[WdmWakeWorkers];
  if (RequestD0(Extension) == STATUS_PENDING)
    Check(Extension,
          KeWaitForSingleObject(&Extension->PowerDone, Executive, KernelMode,
                                FALSE, NULL) == STATUS_SUCCESS,
          InvalidWorker);
  Extension->Work = NULL;
  Extension->Outer = NULL;
  IoFreeWorkItem(Work);
  Complete(Outer, STATUS_SUCCESS, 0);
}

static VOID WakeComplete(PDEVICE_OBJECT Device, UCHAR Minor, POWER_STATE State,
                         PVOID Context, PIO_STATUS_BLOCK IoStatus) {
  WAKE_EXTENSION *Extension = Context;
  const NTSTATUS Status = IoStatus->Status;
  const KIRQL Irql = KeGetCurrentIrql();
  Check(Extension,
        Device == RequestDevice(Extension) && Minor == IRP_MN_WAIT_WAKE &&
            State.SystemState == WakeLimit() && !IoStatus->Information &&
            Extension->WakeIRP &&
            Extension->Values[WdmWakeIoCompletions] ==
                Extension->Values[WdmWakeCallbacks] + 1 &&
            Extension->Values[WdmWakeLastIoIrql] == Irql,
        InvalidCallback);
  ++Extension->Values[WdmWakeCallbacks];
  Extension->Values[WdmWakeLastCallbackIrql] = Irql;
  Extension->WakeIRP = NULL;
  if (Status == STATUS_CANCELLED && Extension->Work) {
    Check(Extension, Irql == DISPATCH_LEVEL, InvalidWorker);
    IoQueueWorkItem(Extension->Work, PowerWorker, DelayedWorkQueue, Extension);
  }
  if (Status == STATUS_CANCELLED &&
      Extension->DpcCommand == WdmWakeCancelDpcDirect) {
    Check(Extension, Irql == DISPATCH_LEVEL && ReadGuestCR8() == Irql,
          InvalidQueuedPower);
    RequestQueuedPower(Extension, IRP_MN_SET_POWER, PowerDeviceD0);
  }
  if (Status == STATUS_SUCCESS) {
    const UCHAR AfterWake = Extension->AfterWake;
    Extension->AfterWake = 0;
    if (AfterWake == WdmWakeRearmOnSuccess)
      ArmWake(Extension);
    else if (AfterWake == WdmWakeD0OnSuccess)
      RequestD0(Extension);
  }
  // A nested request must not reuse the old callback's status storage.
  Check(Extension,
        IoStatus->Status == Status && !IoStatus->Information &&
            KeGetCurrentIrql() == Irql && ReadGuestCR8() == Irql,
        InvalidCallback);
}

static NTSTATUS PowerCompletion(PDEVICE_OBJECT Device, PIRP Irp,
                                PVOID Context) {
  WAKE_EXTENSION *Extension = Context;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  Check(Extension, Device == Extension->Self && !Irp->IoStatus.Information,
        InvalidCompletion);
  if (Stack->MinorFunction == IRP_MN_WAIT_WAKE) {
    Check(Extension,
          Extension->WakeIRP == Irp &&
              Stack->Parameters.WaitWake.PowerState == WakeLimit() &&
              Irp->PendingReturned,
          InvalidCompletion);
    ++Extension->Values[WdmWakeIoCompletions];
    Extension->Values[WdmWakeLastStatus] = Irp->IoStatus.Status;
    Extension->Values[WdmWakeLastIoIrql] = KeGetCurrentIrql();
    if (Mode == WdmWakeModeNoCallback)
      Extension->WakeIRP = NULL;
  }
  if (Extension->PowerContext.Pending &&
      Stack->Parameters.Power.Type == DevicePowerState &&
      Stack->MinorFunction != IRP_MN_WAIT_WAKE)
    Check(Extension, Extension->PowerContext.Irp == Irp, InvalidQueuedPower);
  if (Irp->PendingReturned)
    IoMarkIrpPending(Irp);
  return STATUS_SUCCESS;
}

static NTSTATUS DispatchPower(PDEVICE_OBJECT Device, PIRP Irp) {
  WAKE_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  if (Stack->MinorFunction == IRP_MN_WAIT_WAKE) {
    Check(Extension,
          KeGetCurrentIrql() == PASSIVE_LEVEL &&
              Stack->Parameters.WaitWake.PowerState == WakeLimit(),
          InvalidDispatch);
    if (Mode == WdmWakeModeNoOutput)
      Extension->WakeIRP = Irp;
    else {
      Check(Extension, Extension->WakeIRP == Irp, InvalidDispatch);
      ++Extension->Values[WdmWakePublishedOutputs];
    }
    ++Extension->Values[WdmWakeDispatches];
    Extension->Values[WdmWakeLastLimit] = Stack->Parameters.WaitWake.PowerState;
    Extension->Values[WdmWakeIRPLow] = (ULONG)(ULONG_PTR)Irp;
    Extension->Values[WdmWakeIRPHigh] = (ULONG)((ULONG_PTR)Irp >> 32);
  } else if (Stack->Parameters.Power.Type == DevicePowerState) {
    ++Extension->Values[WdmWakeDeviceDispatches];
    QUEUED_POWER_CONTEXT *Context = &Extension->PowerContext;
    if (Context->Pending) {
      Extension->Values[WdmWakePowerDispatchIrql] = KeGetCurrentIrql();
      Extension->Values[WdmWakePowerDispatchCR8] = ReadGuestCR8();
      Extension->Values[WdmWakePowerDispatchOrder] = ++Extension->Sequence;
      Check(Extension,
            !Context->Irp && Stack->MinorFunction == Context->Minor &&
                Stack->Parameters.Power.State.DeviceState == Context->State &&
                KeGetCurrentIrql() == PASSIVE_LEVEL &&
                ReadGuestCR8() == PASSIVE_LEVEL &&
                Extension->Values[WdmWakeCallerReturnOrder] &&
                Extension->Values[WdmWakePowerDispatchOrder] >
                    Extension->Values[WdmWakeCallerReturnOrder],
            InvalidQueuedPower);
      Context->Irp = Irp;
      ++Extension->Values[WdmWakeQueuedDispatches];
    }
  }
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, PowerCompletion, Extension, TRUE, TRUE, TRUE);
  return PoCallDriver(Extension->Lower, Irp);
}

static VOID CancelWake(WAKE_EXTENSION *Extension) {
  if (!Extension->WakeIRP)
    return;
  const KIRQL Irql = KeGetCurrentIrql();
  ++Extension->Values[WdmWakeCancelCalls];
  const BOOLEAN Cancelled = IoCancelIrp(Extension->WakeIRP);
  Extension->Values[WdmWakeCancelTrue] += Cancelled != FALSE;
  Check(Extension,
        Cancelled && !Extension->WakeIRP && KeGetCurrentIrql() == Irql,
        InvalidCancellation);
}

static VOID CancelDpc(PKDPC Dpc, PVOID Context, PVOID Argument1,
                      PVOID Argument2) {
  WAKE_EXTENSION *Extension = Context;
  UNREFERENCED_PARAMETER(Argument1);
  UNREFERENCED_PARAMETER(Argument2);
  Check(Extension,
        Dpc == &Extension->CancelDpc && KeGetCurrentIrql() == DISPATCH_LEVEL,
        InvalidCancellation);
  switch (Extension->DpcCommand) {
  case WdmWakeCancelDpc:
    CancelWake(Extension);
    return;
  case WdmWakeCancelDpcDirect:
    CancelWake(Extension);
    break;
  case WdmWakeQueryDpc:
  case WdmWakeSetDpc:
    RequestQueuedPower(Extension,
                       Extension->DpcCommand == WdmWakeQueryDpc
                           ? IRP_MN_QUERY_POWER
                           : IRP_MN_SET_POWER,
                       PowerDeviceD2);
    break;
  case WdmWakeWaitWakeDpc: {
    POWER_STATE State;
    State.SystemState = WakeLimit();
    PoRequestPowerIrp(Extension->Self, IRP_MN_WAIT_WAKE, State, WakeComplete,
                      Extension, &Extension->WakeIRP);
    break;
  }
  default:
    Check(Extension, FALSE, InvalidQueuedPower);
    break;
  }
  Check(Extension,
        KeGetCurrentIrql() == DISPATCH_LEVEL &&
            ReadGuestCR8() == DISPATCH_LEVEL,
        InvalidQueuedPower);
  Extension->DpcCommand = 0;
  PIRP Outer = Extension->Outer;
  Extension->Outer = NULL;
  Complete(Outer, STATUS_SUCCESS, 0);
  Extension->Values[WdmWakeCallerReturnOrder] = ++Extension->Sequence;
}

static NTSTATUS PnpCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  WAKE_EXTENSION *Extension = Context;
  Check(Extension,
        Device == Extension->Self && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            !Irp->IoStatus.Information,
        InvalidPnp);
  KeSetEvent(&Extension->PnpDone, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  WAKE_EXTENSION *Extension = Device->DeviceExtension;
  const UCHAR Minor = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
  if (Minor == IRP_MN_QUERY_STOP_DEVICE || Minor == IRP_MN_STOP_DEVICE ||
      Minor == IRP_MN_QUERY_REMOVE_DEVICE || Minor == IRP_MN_REMOVE_DEVICE ||
      Minor == IRP_MN_SURPRISE_REMOVAL)
    CancelWake(Extension);
  if (Minor == IRP_MN_STOP_DEVICE)
    ++Extension->Values[WdmWakeStops];
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    Check(Extension,
          !Extension->WakeIRP && !Extension->Work && !Extension->Outer &&
              !Extension->PowerContext.Pending,
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
    ++Extension->Values[WdmWakeStarts];
    ArmWake(Extension);
  }
  return Complete(Irp, Status, 0);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  WAKE_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Complete(Irp, STATUS_SUCCESS, 0);
  const ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
  const ULONG SnapshotBytes =
      (Code == WdmWakeExtendedSnapshotIoctl ? WdmWakeExtendedSnapshotWords
                                            : WdmWakeSnapshotWords) *
      sizeof(ULONG);
  if ((Code == WdmWakeSnapshotIoctl || Code == WdmWakeExtendedSnapshotIoctl) &&
      Stack->Parameters.DeviceIoControl.OutputBufferLength >= SnapshotBytes) {
    Extension->Values[WdmWakeActive] = Extension->WakeIRP != NULL;
    Extension->Values[WdmWakeHasCancelRoutine] =
        Extension->WakeIRP && Extension->WakeIRP->CancelRoutine != NULL;
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Extension->Values,
                  SnapshotBytes);
    return Complete(Irp, STATUS_SUCCESS, SnapshotBytes);
  }
  if (Code != WdmWakeCommandIoctl ||
      Stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(UCHAR))
    return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
  const UCHAR Command = *(UCHAR *)Irp->AssociatedIrp.SystemBuffer;
  switch (Command) {
  case WdmWakeRearm:
    if (Extension->WakeIRP)
      return Complete(Irp, STATUS_DEVICE_BUSY, 0);
    ArmWake(Extension);
    break;
  case WdmWakeCancel:
    CancelWake(Extension);
    break;
  case WdmWakeCancelRearm:
    CancelWake(Extension);
    ArmWake(Extension);
    break;
  case WdmWakeRearmOnSuccess:
  case WdmWakeD0OnSuccess:
    Extension->AfterWake = Command;
    break;
  case WdmWakeCancelDpc:
    if (!Extension->WakeIRP || Extension->Work || Mode == WdmWakeModeNoCallback)
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    Extension->Work = IoAllocateWorkItem(Device);
    if (!Extension->Work)
      return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    Extension->DpcCommand = Command;
    Extension->Outer = Irp;
    IoMarkIrpPending(Irp);
    Check(Extension, KeInsertQueueDpc(&Extension->CancelDpc, NULL, NULL),
          InvalidCancellation);
    return STATUS_PENDING;
  case WdmWakeCancelDpcDirect:
  case WdmWakeQueryDpc:
  case WdmWakeSetDpc:
  case WdmWakeWaitWakeDpc:
    if (Extension->Outer || Extension->PowerContext.Pending ||
        (Command == WdmWakeCancelDpcDirect &&
         (!Extension->WakeIRP || Mode == WdmWakeModeNoCallback)))
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    if (Command == WdmWakeWaitWakeDpc)
      CancelWake(Extension);
    Extension->DpcCommand = Command;
    Extension->Outer = Irp;
    IoMarkIrpPending(Irp);
    Check(Extension, KeInsertQueueDpc(&Extension->CancelDpc, NULL, NULL),
          InvalidQueuedPower);
    return STATUS_PENDING;
  case WdmWakeQueryApc:
  case WdmWakeSetApc: {
    if (Extension->PowerContext.Pending)
      return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    KIRQL Previous;
    KeRaiseIrql(APC_LEVEL, &Previous);
    RequestQueuedPower(Extension,
                       Command == WdmWakeQueryApc ? IRP_MN_QUERY_POWER
                                                  : IRP_MN_SET_POWER,
                       PowerDeviceD2);
    KeLowerIrql(Previous);
    Extension->Values[WdmWakeCallerReturnOrder] = ++Extension->Sequence;
    break;
  }
  default:
    return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
  }
  return Complete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device = NULL;
  NTSTATUS Status =
      IoCreateDevice(Driver, sizeof(WAKE_EXTENSION), NULL, FILE_DEVICE_UNKNOWN,
                     FILE_DEVICE_SECURE_OPEN, FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  WAKE_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  if (!Extension->Lower) {
    IoDeleteDevice(Device);
    return STATUS_NO_SUCH_DEVICE;
  }
  KeInitializeEvent(&Extension->PnpDone, NotificationEvent, FALSE);
  KeInitializeEvent(&Extension->PowerDone, NotificationEvent, FALSE);
  KeInitializeDpc(&Extension->CancelDpc, CancelDpc, Extension);
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  ++LiveDevices;
  return STATUS_SUCCESS;
}

static VOID Unload(PDRIVER_OBJECT Driver) {
  if (LiveDevices || Driver->DeviceObject)
    Failures |= InvalidUnload;
  DbgPrint("WDM wait wake: unload live %lu failures %lu\n", LiveDevices,
           Failures);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  Mode = WdmWakeModeWorking;
  if (RegistryPath->Length >= sizeof(WCHAR))
    Mode =
        (UCHAR)RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1];
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_POWER] = DispatchPower;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  return STATUS_SUCCESS;
}

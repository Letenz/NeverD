//===- driver_kmdf_usb_idle.c - Genuine KMDF USB selective suspend -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// The driver supplies policy and ordinary WDF callbacks. The framework owns
/// the USB idle request; this fixture never allocates or submits that packet.
//===----------------------------------------------------------------------===//
#include "driver_kmdf_usb_idle_test.h"

#include <ntifs.h>
#include <wdf.h>

#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")
ABI_SLOT(WdfDeviceAssignS0IdleSettings, 46);
ABI_SLOT(WdfDeviceInitSetPowerPolicyEventCallbacks, 56);
ABI_SLOT(WdfDeviceStopIdleNoTrack, 88);
ABI_SLOT(WdfDeviceResumeIdleNoTrack, 89);
_Static_assert(sizeof(WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS) == 36,
               "KMDF 1.33 idle settings layout");
_Static_assert(sizeof(WDF_POWER_POLICY_EVENT_CALLBACKS) == 64,
               "KMDF 1.33 policy callback layout");

enum {
  InvalidEntry = 1U << 0,
  InvalidExit = 1U << 1,
  InvalidArm = 1U << 2,
  InvalidDisarm = 1U << 3,
  InvalidTrigger = 1U << 4,
  InvalidRead = 1U << 5,
  InvalidStop = 1U << 6,
  InvalidCleanup = 1U << 7
};

typedef struct {
  ULONG Values[KmdfUsbSnapshotWords];
  ULONG Sequence;
  BOOLEAN FailArm;
  BOOLEAN StopInArm;
  BOOLEAN ArmAttempt;
  BOOLEAN StopHeld;
  BOOLEAN WorkPending;
  WDFQUEUE ManagedQueue;
  PIO_WORKITEM WorkItem;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceContext);

static ULONG LiveDevices;
static ULONG Failures;

static VOID Check(DEVICE_CONTEXT *Context, BOOLEAN Condition, ULONG Failure) {
  if (!Condition) {
    Context->Values[KmdfUsbFailures] |= Failure;
    Failures |= Failure;
    DbgPrint("KMDF USB idle: invalid contract %lu\n", Failure);
  }
}

static NTSTATUS D0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL && !Context->Values[KmdfUsbInD0],
        InvalidEntry);
  ++Context->Values[KmdfUsbD0Entries];
  Context->Values[KmdfUsbPreviousState] = Previous;
  Context->Values[KmdfUsbInD0] = TRUE;
  Context->Values[KmdfUsbEntrySequence] = ++Context->Sequence;
  return STATUS_SUCCESS;
}

static NTSTATUS D0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL && Context->Values[KmdfUsbInD0],
        InvalidExit);
  ++Context->Values[KmdfUsbD0Exits];
  Context->Values[KmdfUsbTargetState] = Target;
  Context->Values[KmdfUsbInD0] = FALSE;
  ++Context->Sequence;
  return STATUS_SUCCESS;
}

static NTSTATUS StopIdle(WDFDEVICE Device) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context, !Context->StopHeld, InvalidStop);
  NTSTATUS Status = WdfDeviceStopIdle(Device, FALSE);
  ++Context->Values[KmdfUsbStopCalls];
  Context->Values[KmdfUsbStopStatus] = Status;
  if (NT_SUCCESS(Status))
    Context->StopHeld = TRUE;
  return Status;
}

static VOID StopWorker(PDEVICE_OBJECT WdmDevice, PVOID Argument) {
  WDFDEVICE Device = (WDFDEVICE)Argument;
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL && Context->WorkPending &&
            WdmDevice == WdfDeviceWdmGetDeviceObject(Device),
        InvalidStop);
  Context->WorkPending = FALSE;
  StopIdle(Device);
}

static NTSTATUS ArmWake(WDFDEVICE Device) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL && Context->Values[KmdfUsbInD0] &&
            !Context->ArmAttempt,
        InvalidArm);
  Context->ArmAttempt = TRUE;
  ++Context->Values[KmdfUsbArms];
  if (Context->FailArm)
    return STATUS_UNSUCCESSFUL;
  Context->Values[KmdfUsbArmed] = TRUE;
  if (Context->StopInArm) {
    Context->WorkPending = TRUE;
    IoQueueWorkItem(Context->WorkItem, StopWorker, DelayedWorkQueue, Device);
  }
  return STATUS_SUCCESS;
}

static VOID DisarmWake(WDFDEVICE Device) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context, KeGetCurrentIrql() == PASSIVE_LEVEL && Context->ArmAttempt,
        InvalidDisarm);
  ++Context->Values[KmdfUsbDisarms];
  Context->ArmAttempt = FALSE;
  Context->Values[KmdfUsbArmed] = FALSE;
}

static VOID WakeTriggered(WDFDEVICE Device) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL && Context->Values[KmdfUsbInD0] &&
            Context->Values[KmdfUsbArmed],
        InvalidTrigger);
  ++Context->Values[KmdfUsbTriggers];
}

static VOID ManagedRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length) {
  DEVICE_CONTEXT *Context = DeviceContext(WdfIoQueueGetDevice(Queue));
  PULONG Output = NULL;
  Check(Context,
        Queue == Context->ManagedQueue && KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Context->Values[KmdfUsbInD0],
        InvalidRead);
  ++Context->Values[KmdfUsbReadsDelivered];
  Context->Values[KmdfUsbReadDeliverySequence] = ++Context->Sequence;
  NTSTATUS Status = Length >= sizeof(*Output)
                        ? WdfRequestRetrieveOutputBuffer(
                              Request, sizeof(*Output), (PVOID *)&Output, NULL)
                        : STATUS_BUFFER_TOO_SMALL;
  if (NT_SUCCESS(Status))
    *Output = KmdfUsbReadMarker;
  WdfRequestCompleteWithInformation(Request, Status,
                                    NT_SUCCESS(Status) ? sizeof(*Output) : 0);
}

static VOID RouteRead(WDFQUEUE Queue, WDFREQUEST Request, size_t Length) {
  UNREFERENCED_PARAMETER(Length);
  DEVICE_CONTEXT *Context = DeviceContext(WdfIoQueueGetDevice(Queue));
  ++Context->Values[KmdfUsbReadsRouted];
  Context->Values[KmdfUsbReadRouteSequence] = ++Context->Sequence;
  NTSTATUS Status = WdfRequestForwardToIoQueue(Request, Context->ManagedQueue);
  if (!NT_SUCCESS(Status))
    WdfRequestComplete(Request, Status);
}

static NTSTATUS Configure(WDFDEVICE Device, WDFREQUEST Request) {
  PUCHAR Input = NULL;
  NTSTATUS Status = WdfRequestRetrieveInputBuffer(
      Request, KmdfUsbConfigurationBytes, (PVOID *)&Input, NULL);
  if (!NT_SUCCESS(Status))
    return Status;
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Context->Values[KmdfUsbIdentity] = Input[KmdfUsbConfigIdentity];
  Context->FailArm = Input[KmdfUsbConfigFailArm] != 0;
  Context->StopInArm = Input[KmdfUsbConfigStopInArm] != 0;
  WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS Settings;
  WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&Settings,
                                             IdleUsbSelectiveSuspend);
  if (!Input[KmdfUsbConfigMaximum])
    Settings.DxState = PowerDeviceD2;
  Settings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
  Settings.IdleTimeout = KmdfUsbIdleTimeoutMilliseconds;
  Settings.Enabled = Input[KmdfUsbConfigEnabled] ? WdfTrue : WdfFalse;
  Settings.IdleTimeoutType = DriverManagedIdleTimeout;
  Settings.ExcludeD3Cold = WdfTrue;
  return WdfDeviceAssignS0IdleSettings(Device, &Settings);
}

static VOID IoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                      size_t InputLength, ULONG Code) {
  UNREFERENCED_PARAMETER(InputLength);
  WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  NTSTATUS Status;
  if (Code == KmdfUsbSnapshotIoctl) {
    PVOID Output = NULL;
    Status = OutputLength >= sizeof(Context->Values)
                 ? WdfRequestRetrieveOutputBuffer(
                       Request, sizeof(Context->Values), &Output, NULL)
                 : STATUS_BUFFER_TOO_SMALL;
    if (NT_SUCCESS(Status))
      RtlCopyMemory(Output, Context->Values, sizeof(Context->Values));
    WdfRequestCompleteWithInformation(
        Request, Status, NT_SUCCESS(Status) ? sizeof(Context->Values) : 0);
    return;
  }
  if (Code == KmdfUsbConfigureIoctl)
    Status = Configure(Device, Request);
  else if (Code == KmdfUsbStopIdleIoctl)
    Status = StopIdle(Device);
  else if (Code == KmdfUsbResumeIdleIoctl) {
    Check(Context, Context->StopHeld, InvalidStop);
    WdfDeviceResumeIdle(Device);
    Context->StopHeld = FALSE;
    ++Context->Values[KmdfUsbResumeCalls];
    Status = STATUS_SUCCESS;
  } else
    Status = STATUS_INVALID_DEVICE_REQUEST;
  WdfRequestComplete(Request, Status);
}

static VOID Cleanup(WDFOBJECT Object) {
  DEVICE_CONTEXT *Context = DeviceContext(Object);
  Check(Context, LiveDevices && !Context->WorkPending && !Context->StopHeld,
        InvalidCleanup);
  if (Context->WorkItem != NULL)
    IoFreeWorkItem(Context->WorkItem);
  --LiveDevices;
}

static NTSTATUS DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
  UNREFERENCED_PARAMETER(Driver);
  WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
  WDF_POWER_POLICY_EVENT_CALLBACKS Policy;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_IO_QUEUE_CONFIG Queue;
  WDFDEVICE Device;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
  Pnp.EvtDeviceD0Entry = D0Entry;
  Pnp.EvtDeviceD0Exit = D0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);
  WDF_POWER_POLICY_EVENT_CALLBACKS_INIT(&Policy);
  Policy.EvtDeviceArmWakeFromS0 = ArmWake;
  Policy.EvtDeviceDisarmWakeFromS0 = DisarmWake;
  Policy.EvtDeviceWakeFromS0Triggered = WakeTriggered;
  WdfDeviceInitSetPowerPolicyEventCallbacks(Init, &Policy);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, DEVICE_CONTEXT);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Attributes.EvtCleanupCallback = Cleanup;
  NTSTATUS Status = WdfDeviceCreate(&Init, &Attributes, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  ++LiveDevices;
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Context->WorkItem = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(Device));
  if (Context->WorkItem == NULL)
    return STATUS_INSUFFICIENT_RESOURCES;
  WDF_IO_QUEUE_CONFIG_INIT(&Queue, WdfIoQueueDispatchSequential);
  Queue.PowerManaged = WdfTrue;
  Queue.EvtIoRead = ManagedRead;
  Status = WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES,
                            &Context->ManagedQueue);
  if (!NT_SUCCESS(Status))
    return Status;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchSequential);
  Queue.PowerManaged = WdfFalse;
  Queue.EvtIoRead = RouteRead;
  Queue.EvtIoDeviceControl = IoControl;
  return WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES,
                          WDF_NO_HANDLE);
}

static VOID DriverUnload(WDFDRIVER Driver) {
  UNREFERENCED_PARAMETER(Driver);
  DbgPrint("KMDF USB idle: unload live %lu failures %lu\n", LiveDevices,
           Failures);
  if (LiveDevices || Failures)
    ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config;
  WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);
  Config.EvtDriverUnload = DriverUnload;
  return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                         &Config, WDF_NO_HANDLE);
}

//===- driver_kmdf_child_wake.c - Genuine WDK parent and child wake ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Give every configured device independent typed state and a queue that can
/// observe Dx without requesting D0. Scenario topology supplies the parent and
/// child relationships; an IOCTL configures each device's wake policy.
///
//===----------------------------------------------------------------------===//

#include "driver_kmdf_child_wake_test.h"

#include <ntifs.h>
#include <wdf.h>

#define ABI_OFFSET(Type, Member, Offset)                                       \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset, #Type "." #Member)
#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")

_Static_assert(sizeof(WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS) == 20,
               "KMDF x64 wake settings layout");
ABI_OFFSET(WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS,
           ArmForWakeIfChildrenAreArmedForWake, 16);
ABI_OFFSET(WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS, IndicateChildWakeOnParentWake,
           17);
ABI_OFFSET(WDF_POWER_POLICY_EVENT_CALLBACKS, EvtDeviceArmWakeFromSxWithReason,
           56);
ABI_SLOT(WdfDeviceAssignSxWakeSettings, 47);
ABI_SLOT(WdfDeviceInitSetPowerPolicyEventCallbacks, 56);

_Static_assert(KmdfChildWakeSnapshotIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN,
                                                      0x800, METHOD_BUFFERED,
                                                      FILE_ANY_ACCESS),
               "snapshot control code");
_Static_assert(KmdfChildWakeConfigureIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN,
                                                       0x801, METHOD_BUFFERED,
                                                       FILE_ANY_ACCESS),
               "configuration control code");

enum {
  InvalidEntry = 1U << 0,
  InvalidExit = 1U << 1,
  InvalidArm = 1U << 2,
  InvalidTriggered = 1U << 3,
  InvalidDisarm = 1U << 4,
  InvalidCleanup = 1U << 5
};

typedef struct {
  ULONG Values[KmdfChildWakeSnapshotWords];
  BOOLEAN FailArm;
  BOOLEAN OwnEnabled;
  BOOLEAN ArmAttemptPending;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceContext);

static ULONG LiveDevices;
static ULONG Failures;

static VOID Check(DEVICE_CONTEXT *Context, BOOLEAN Condition, ULONG Failure) {
  if (!Condition) {
    Context->Values[KmdfChildWakeFailures] |= Failure;
    Failures |= Failure;
    DbgPrint("KMDF child wake: invalid contract %lu\n", Failure);
  }
}

static NTSTATUS D0Entry(WDFDEVICE Device,
                        WDF_POWER_DEVICE_STATE PreviousState) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  UNREFERENCED_PARAMETER(PreviousState);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
            !Context->Values[KmdfChildWakeInD0],
        InvalidEntry);
  ++Context->Values[KmdfChildWakeD0Entries];
  Context->Values[KmdfChildWakeInD0] = TRUE;
  return STATUS_SUCCESS;
}

static NTSTATUS D0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  UNREFERENCED_PARAMETER(TargetState);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Context->Values[KmdfChildWakeInD0],
        InvalidExit);
  ++Context->Values[KmdfChildWakeD0Exits];
  Context->Values[KmdfChildWakeInD0] = FALSE;
  return STATUS_SUCCESS;
}

static NTSTATUS ArmWake(WDFDEVICE Device, BOOLEAN DeviceWakeEnabled,
                        BOOLEAN ChildrenArmedForWake) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Context->Values[KmdfChildWakeInD0] &&
            !Context->Values[KmdfChildWakeArmed] &&
            !Context->ArmAttemptPending &&
            DeviceWakeEnabled == Context->OwnEnabled &&
            (DeviceWakeEnabled || ChildrenArmedForWake),
        InvalidArm);
  ++Context->Values[KmdfChildWakeArms];
  Context->Values[KmdfChildWakeOwnReason] = DeviceWakeEnabled;
  Context->Values[KmdfChildWakeChildrenReason] = ChildrenArmedForWake;
  Context->ArmAttemptPending = TRUE;
  if (Context->FailArm)
    return STATUS_UNSUCCESSFUL;
  Context->Values[KmdfChildWakeArmed] = TRUE;
  return STATUS_SUCCESS;
}

static VOID WakeTriggered(WDFDEVICE Device) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL &&
            Context->Values[KmdfChildWakeInD0] &&
            Context->Values[KmdfChildWakeArmed],
        InvalidTriggered);
  ++Context->Values[KmdfChildWakeTriggers];
}

static VOID DisarmWake(WDFDEVICE Device) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  Check(Context,
        KeGetCurrentIrql() == PASSIVE_LEVEL && Context->ArmAttemptPending,
        InvalidDisarm);
  ++Context->Values[KmdfChildWakeDisarms];
  Context->Values[KmdfChildWakeArmed] = FALSE;
  Context->ArmAttemptPending = FALSE;
}

static NTSTATUS Configure(WDFDEVICE Device, const UCHAR *Input) {
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS Wake;
  NTSTATUS Status;
  if (Context->Values[KmdfChildWakeArmed] || Context->ArmAttemptPending ||
      Input[KmdfChildWakeOwnEnabled] > TRUE ||
      Input[KmdfChildWakeArmForChildren] > TRUE ||
      Input[KmdfChildWakePropagate] > TRUE ||
      Input[KmdfChildWakeFailArm] > TRUE)
    return STATUS_INVALID_PARAMETER;
  WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&Wake);
  Wake.DxState = PowerDeviceD3;
  Wake.UserControlOfWakeSettings = WakeDoNotAllowUserControl;
  Wake.Enabled = Input[KmdfChildWakeOwnEnabled] ? WdfTrue : WdfFalse;
  Wake.ArmForWakeIfChildrenAreArmedForWake = Input[KmdfChildWakeArmForChildren];
  Wake.IndicateChildWakeOnParentWake = Input[KmdfChildWakePropagate];
  Status = WdfDeviceAssignSxWakeSettings(Device, &Wake);
  if (NT_SUCCESS(Status)) {
    Context->Values[KmdfChildWakeDeviceIdentity] = Input[KmdfChildWakeIdentity];
    Context->OwnEnabled = Input[KmdfChildWakeOwnEnabled];
    Context->FailArm = Input[KmdfChildWakeFailArm];
  }
  return Status;
}

static VOID IoControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength,
                      size_t InputLength, ULONG Code) {
  WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
  DEVICE_CONTEXT *Context = DeviceContext(Device);
  NTSTATUS Status;
  if (Code == KmdfChildWakeConfigureIoctl) {
    UCHAR *Input;
    if (InputLength < KmdfChildWakeConfigurationBytes) {
      WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
      return;
    }
    Status = WdfRequestRetrieveInputBuffer(
        Request, KmdfChildWakeConfigurationBytes, (PVOID *)&Input, NULL);
    if (NT_SUCCESS(Status))
      Status = Configure(Device, Input);
    WdfRequestComplete(Request, Status);
    return;
  }
  if (Code == KmdfChildWakeSnapshotIoctl) {
    ULONG *Output;
    if (OutputLength < sizeof(Context->Values)) {
      WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
      return;
    }
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(Context->Values),
                                            (PVOID *)&Output, NULL);
    if (!NT_SUCCESS(Status)) {
      WdfRequestComplete(Request, Status);
      return;
    }
    RtlCopyMemory(Output, Context->Values, sizeof(Context->Values));
    WdfRequestCompleteWithInformation(Request,
                                      Context->Values[KmdfChildWakeFailures]
                                          ? STATUS_INVALID_DEVICE_STATE
                                          : STATUS_SUCCESS,
                                      sizeof(Context->Values));
    return;
  }
  WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
}

static VOID DeviceCleanup(WDFOBJECT Object) {
  DEVICE_CONTEXT *Context = DeviceContext(Object);
  Check(Context,
        !Context->Values[KmdfChildWakeArmed] && !Context->ArmAttemptPending &&
            LiveDevices != 0,
        InvalidCleanup);
  --LiveDevices;
}

static NTSTATUS DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
  WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
  WDF_POWER_POLICY_EVENT_CALLBACKS Policy;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_IO_QUEUE_CONFIG Queue;
  WDFDEVICE Device;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Driver);
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
  Pnp.EvtDeviceD0Entry = D0Entry;
  Pnp.EvtDeviceD0Exit = D0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);
  WDF_POWER_POLICY_EVENT_CALLBACKS_INIT(&Policy);
  Policy.EvtDeviceArmWakeFromSxWithReason = ArmWake;
  Policy.EvtDeviceWakeFromSxTriggered = WakeTriggered;
  Policy.EvtDeviceDisarmWakeFromSx = DisarmWake;
  WdfDeviceInitSetPowerPolicyEventCallbacks(Init, &Policy);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, DEVICE_CONTEXT);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Attributes.EvtCleanupCallback = DeviceCleanup;
  Status = WdfDeviceCreate(&Init, &Attributes, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  ++LiveDevices;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchSequential);
  Queue.PowerManaged = WdfFalse;
  Queue.EvtIoDeviceControl = IoControl;
  return WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES,
                          WDF_NO_HANDLE);
}

static VOID DriverUnload(WDFDRIVER Driver) {
  UNREFERENCED_PARAMETER(Driver);
  DbgPrint("KMDF child wake: unload live %lu failures %lu\n", LiveDevices,
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

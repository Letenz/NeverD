//===- driver_kmdf_pofx.c - Genuine WDK single-component Fx fixture -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Service suffix N uses all component callbacks, A acknowledges idle from a
/// worker, O supplies only the F-state callback, S assigns settings during
/// SelfManagedIoInit, F fails PostRegister, and Z requests the default F0-only
/// component. The scenario supplies real idle, F-state and device-power events.
///
//===----------------------------------------------------------------------===//

#include "driver_kmdf_pofx_test.h"

#include <ntifs.h>
#include <wdf.h>

#define ABI_OFFSET(Type, Member, Offset)                                       \
  _Static_assert(__builtin_offsetof(Type, Member) == Offset, #Type "." #Member)
#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")

_Static_assert(sizeof(WDF_POWER_FRAMEWORK_SETTINGS) == 88,
               "KMDF 1.33 power framework settings layout");
_Static_assert(sizeof(PO_FX_COMPONENT) == 32 &&
                   sizeof(PO_FX_COMPONENT_IDLE_STATE) == 24,
               "x64 component and idle-state layout");
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, EvtDeviceWdmPostPoFxRegisterDevice, 8);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, EvtDeviceWdmPrePoFxUnregisterDevice,
           16);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, Component, 24);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, ComponentActiveConditionCallback, 32);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, ComponentIdleConditionCallback, 40);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, ComponentIdleStateCallback, 48);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, PowerControlCallback, 56);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, PoFxDeviceContext, 64);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, PoFxDeviceFlags, 72);
ABI_OFFSET(WDF_POWER_FRAMEWORK_SETTINGS, DirectedPoFxEnabled, 80);
ABI_OFFSET(PO_FX_COMPONENT, IdleStateCount, 16);
ABI_OFFSET(PO_FX_COMPONENT, DeepestWakeableIdleState, 20);
ABI_OFFSET(PO_FX_COMPONENT, IdleStates, 24);
ABI_SLOT(WdfDeviceWdmAssignPowerFrameworkSettings, 425);
ABI_SLOT(WdfDeviceAssignS0IdleSettings, 46);
ABI_SLOT(WdfIoQueueStop, 155);
ABI_SLOT(WdfIoQueueStart, 154);

enum {
  ComponentIndex = 0,
  ActiveState = 0,
  LowPowerState = 1,
  StateCount = 2,
  LowStateLatency = 10,
  LowStateResidency = 20,
  ActiveStatePower = 100,
  LowStatePower = 25
};

enum {
  InvalidPostRegister = 1U << 0,
  InvalidPreUnregister = 1U << 1,
  InvalidIdleCondition = 1U << 2,
  InvalidActiveCondition = 1U << 3,
  InvalidIdleState = 1U << 4,
  InvalidQueueStop = 1U << 5,
  InvalidWorker = 1U << 6,
  InvalidD0Entry = 1U << 7,
  InvalidD0Exit = 1U << 8,
  InvalidRequest = 1U << 9,
  InvalidCleanup = 1U << 10,
  InvalidUnload = 1U << 11
};

typedef struct {
  ULONG Failures;
  ULONG PostRegistrations;
  ULONG PreUnregistrations;
  ULONG ActiveConditions;
  ULONG IdleConditions;
  ULONG F0Transitions;
  ULONG F1Transitions;
  ULONG CurrentState;
  ULONG D0Entries;
  ULONG D0Exits;
} POWER_SNAPSHOT;
_Static_assert(sizeof(POWER_SNAPSHOT) == KmdfPoFxSnapshotWords * sizeof(ULONG),
               "shared PoFx snapshot layout");
_Static_assert(KmdfPoFxSnapshotIoctl == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800,
                                                 METHOD_BUFFERED,
                                                 FILE_ANY_ACCESS),
               "shared PoFx snapshot control code");

static WCHAR ServiceMode;
static WDFDEVICE Device;
static WDFQUEUE Queue;
static POHANDLE PowerHandle;
static PIO_WORKITEM IdleWorkItem;
static POWER_SNAPSHOT Snapshot;
static BOOLEAN InD0;
static BOOLEAN IdleAcknowledgementPending;
static BOOLEAN QueueStoppedForIdle;

static VOID Check(BOOLEAN Condition, ULONG Failure) {
  if (!Condition) {
    Snapshot.Failures |= Failure;
    DbgPrint("KMDF PoFx: invalid contract %lu\n", Failure);
  }
}

static BOOLEAN HasConditionCallbacks(VOID) {
  return ServiceMode != KmdfPoFxDefaultConditions &&
         ServiceMode != KmdfPoFxDefaultComponent;
}

static VOID CompleteIdle(VOID) {
  Check(PowerHandle != NULL && IdleAcknowledgementPending &&
            QueueStoppedForIdle,
        InvalidIdleCondition);
  IdleAcknowledgementPending = FALSE;
  PoFxCompleteIdleCondition(PowerHandle, ComponentIndex);
  DbgPrint("KMDF PoFx: idle acknowledged\n");
}

static VOID IdleWorker(PDEVICE_OBJECT WdmDevice, PVOID Context) {
  Check(WdmDevice == WdfDeviceWdmGetDeviceObject(Device) && Context == Device &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidWorker);
  CompleteIdle();
}

static VOID QueueStopped(WDFQUEUE StoppedQueue, WDFCONTEXT Context) {
  Check(StoppedQueue == Queue && Context == Device &&
            IdleAcknowledgementPending,
        InvalidQueueStop);
  QueueStoppedForIdle = TRUE;
  if (ServiceMode == KmdfPoFxDeferredIdle) {
    IoQueueWorkItem(IdleWorkItem, IdleWorker, DelayedWorkQueue, Device);
    return;
  }
  CompleteIdle();
}

static VOID ComponentIdle(PVOID Context, ULONG Component) {
  Check(Context == Device && Component == ComponentIndex &&
            PowerHandle != NULL && !IdleAcknowledgementPending &&
            KeGetCurrentIrql() <= DISPATCH_LEVEL,
        InvalidIdleCondition);
  ++Snapshot.IdleConditions;
  IdleAcknowledgementPending = TRUE;
  DbgPrint("KMDF PoFx: idle condition\n");
  WdfIoQueueStop(Queue, QueueStopped, Device);
}

static VOID ComponentActive(PVOID Context, ULONG Component) {
  Check(Context == Device && Component == ComponentIndex &&
            PowerHandle != NULL && InD0 &&
            Snapshot.CurrentState == ActiveState &&
            !IdleAcknowledgementPending && KeGetCurrentIrql() <= DISPATCH_LEVEL,
        InvalidActiveCondition);
  ++Snapshot.ActiveConditions;
  DbgPrint("KMDF PoFx: active condition\n");
  if (QueueStoppedForIdle) {
    QueueStoppedForIdle = FALSE;
    WdfIoQueueStart(Queue);
  }
}

static VOID ComponentState(PVOID Context, ULONG Component, ULONG State) {
  Check(Context == Device && Component == ComponentIndex &&
            PowerHandle != NULL && State < StateCount &&
            !IdleAcknowledgementPending && InD0 &&
            KeGetCurrentIrql() <= DISPATCH_LEVEL,
        InvalidIdleState);
  Snapshot.CurrentState = State;
  if (State == ActiveState)
    ++Snapshot.F0Transitions;
  else
    ++Snapshot.F1Transitions;
  DbgPrint("KMDF PoFx: state F%lu\n", State);
  PoFxCompleteIdleState(PowerHandle, Component);
}

static NTSTATUS PostRegister(WDFDEVICE RegisteredDevice, POHANDLE Handle) {
  Check(RegisteredDevice == Device && Handle != NULL && PowerHandle == NULL &&
            InD0 && KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidPostRegister);
  PowerHandle = Handle;
  ++Snapshot.PostRegistrations;
  Snapshot.CurrentState = ActiveState;
  PoFxSetComponentLatency(Handle, ComponentIndex, LowStateLatency);
  PoFxSetComponentResidency(Handle, ComponentIndex, LowStateResidency);
  PoFxSetComponentWake(Handle, ComponentIndex, TRUE);
  DbgPrint("KMDF PoFx: post register %lu\n", Snapshot.PostRegistrations);
  return ServiceMode == KmdfPoFxFailPostRegister ? STATUS_UNSUCCESSFUL
                                                 : STATUS_SUCCESS;
}

static VOID PreUnregister(WDFDEVICE RegisteredDevice, POHANDLE Handle) {
  Check(RegisteredDevice == Device && Handle == PowerHandle && Handle != NULL &&
            !IdleAcknowledgementPending && KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidPreUnregister);
  // The POHANDLE remains valid for the entire pre-unregister callback.
  PoFxSetComponentWake(Handle, ComponentIndex, FALSE);
  ++Snapshot.PreUnregistrations;
  DbgPrint("KMDF PoFx: pre unregister %lu\n", Snapshot.PreUnregistrations);
  PowerHandle = NULL;
}

static NTSTATUS AssignFrameworkSettings(VOID) {
  PO_FX_COMPONENT_IDLE_STATE States[StateCount] = {
      {0, 0, ActiveStatePower},
      {LowStateLatency, LowStateResidency, LowStatePower}};
  PO_FX_COMPONENT Component = {0};
  WDF_POWER_FRAMEWORK_SETTINGS Settings;
  Component.IdleStateCount = StateCount;
  Component.DeepestWakeableIdleState = LowPowerState;
  Component.IdleStates = States;
  WDF_POWER_FRAMEWORK_SETTINGS_INIT(&Settings);
  Settings.EvtDeviceWdmPostPoFxRegisterDevice = PostRegister;
  Settings.EvtDeviceWdmPrePoFxUnregisterDevice = PreUnregister;
  Settings.PoFxDeviceContext = Device;
  Settings.DirectedPoFxEnabled = WdfFalse;
  if (ServiceMode != KmdfPoFxDefaultComponent) {
    Settings.Component = &Component;
    Settings.ComponentIdleStateCallback = ComponentState;
  }
  if (HasConditionCallbacks()) {
    Settings.ComponentActiveConditionCallback = ComponentActive;
    Settings.ComponentIdleConditionCallback = ComponentIdle;
  }
  // Stack-backed descriptions test that settings and F-state metadata are
  // copied.
  return WdfDeviceWdmAssignPowerFrameworkSettings(Device, &Settings);
}

static NTSTATUS SelfManagedIoInit(WDFDEVICE InitializedDevice) {
  if (InitializedDevice != Device || !InD0)
    return STATUS_INVALID_DEVICE_STATE;
  if (ServiceMode == KmdfPoFxSelfManagedSettings)
    return AssignFrameworkSettings();
  return STATUS_SUCCESS;
}

static NTSTATUS D0Entry(WDFDEVICE EnteringDevice,
                        WDF_POWER_DEVICE_STATE PreviousState) {
  UNREFERENCED_PARAMETER(PreviousState);
  Check(EnteringDevice == Device && !InD0 &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidD0Entry);
  InD0 = TRUE;
  ++Snapshot.D0Entries;
  DbgPrint("KMDF PoFx: D0 entry\n");
  return STATUS_SUCCESS;
}

static NTSTATUS D0Exit(WDFDEVICE ExitingDevice,
                       WDF_POWER_DEVICE_STATE TargetState) {
  UNREFERENCED_PARAMETER(TargetState);
  Check(ExitingDevice == Device && InD0 && !IdleAcknowledgementPending &&
            KeGetCurrentIrql() == PASSIVE_LEVEL,
        InvalidD0Exit);
  InD0 = FALSE;
  ++Snapshot.D0Exits;
  DbgPrint("KMDF PoFx: D0 exit\n");
  return STATUS_SUCCESS;
}

static VOID IoControl(WDFQUEUE RequestQueue, WDFREQUEST Request,
                      size_t OutputLength, size_t InputLength, ULONG Code) {
  POWER_SNAPSHOT *Output;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(InputLength);
  Check(RequestQueue == Queue && PowerHandle != NULL && InD0 &&
            Snapshot.CurrentState == ActiveState &&
            !IdleAcknowledgementPending && !QueueStoppedForIdle,
        InvalidRequest);
  if (Code != KmdfPoFxSnapshotIoctl) {
    WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
    return;
  }
  if (OutputLength < sizeof(*Output)) {
    WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
    return;
  }
  Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Output),
                                          (PVOID *)&Output, NULL);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  *Output = Snapshot;
  WdfRequestCompleteWithInformation(
      Request, Snapshot.Failures ? STATUS_INVALID_DEVICE_STATE : STATUS_SUCCESS,
      sizeof(*Output));
}

static VOID DeviceCleanup(WDFOBJECT Object) {
  Check(Object == Device && PowerHandle == NULL && !IdleAcknowledgementPending,
        InvalidCleanup);
  if (IdleWorkItem != NULL) {
    IoFreeWorkItem(IdleWorkItem);
    IdleWorkItem = NULL;
  }
  DbgPrint("KMDF PoFx: device cleanup\n");
}

static NTSTATUS DeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_PNPPOWER_EVENT_CALLBACKS Callbacks;
  WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;
  WDF_IO_QUEUE_CONFIG QueueConfig;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Driver);
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Callbacks);
  Callbacks.EvtDeviceD0Entry = D0Entry;
  Callbacks.EvtDeviceD0Exit = D0Exit;
  Callbacks.EvtDeviceSelfManagedIoInit = SelfManagedIoInit;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Callbacks);
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Attributes.EvtCleanupCallback = DeviceCleanup;
  Status = WdfDeviceCreate(&Init, &Attributes, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings,
                                             IdleCannotWakeFromS0);
  IdleSettings.DxState = PowerDeviceD3;
  IdleSettings.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
  IdleSettings.IdleTimeout = KmdfPoFxIdleTimeoutMilliseconds;
  IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
  IdleSettings.ExcludeD3Cold = WdfTrue;
  Status = WdfDeviceAssignS0IdleSettings(Device, &IdleSettings);
  if (!NT_SUCCESS(Status))
    return Status;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig,
                                         WdfIoQueueDispatchSequential);
  QueueConfig.PowerManaged = WdfTrue;
  QueueConfig.EvtIoDeviceControl = IoControl;
  Status =
      WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Queue);
  if (!NT_SUCCESS(Status))
    return Status;
  if (ServiceMode == KmdfPoFxDeferredIdle) {
    IdleWorkItem = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(Device));
    if (IdleWorkItem == NULL)
      return STATUS_INSUFFICIENT_RESOURCES;
  }
  if (ServiceMode == KmdfPoFxSelfManagedSettings)
    return STATUS_SUCCESS;
  return AssignFrameworkSettings();
}

static VOID DriverUnload(WDFDRIVER Driver) {
  UNREFERENCED_PARAMETER(Driver);
  Check(PowerHandle == NULL && IdleWorkItem == NULL &&
            Snapshot.PostRegistrations == Snapshot.PreUnregistrations,
        InvalidUnload);
  DbgPrint("KMDF PoFx: unload failures %lu\n", Snapshot.Failures);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config;
  ServiceMode =
      RegistryPath->Length >= sizeof(WCHAR)
          ? RegistryPath->Buffer[RegistryPath->Length / sizeof(WCHAR) - 1]
          : KmdfPoFxNormal;
  WDF_DRIVER_CONFIG_INIT(&Config, DeviceAdd);
  Config.EvtDriverUnload = DriverUnload;
  return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                         &Config, WDF_NO_HANDLE);
}

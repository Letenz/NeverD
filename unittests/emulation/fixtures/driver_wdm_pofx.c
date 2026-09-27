//===- driver_wdm_pofx.c - Genuine WDK component power fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise original WDM component-power callbacks using genuine WDK contracts.
///
//===----------------------------------------------------------------------===//

#include <ntddk.h>

#define ABI_OFFSET(Type, Member, Value)                                        \
  _Static_assert(__builtin_offsetof(Type, Member) == Value, #Type "." #Member)
_Static_assert(sizeof(PO_FX_DEVICE_V1) == 96 &&
                   sizeof(PO_FX_COMPONENT_V1) == 32 &&
                   sizeof(PO_FX_COMPONENT_IDLE_STATE) == 24,
               "native x64 PoFx version 1 structures");
ABI_OFFSET(PO_FX_DEVICE_V1, ComponentActiveConditionCallback, 8);
ABI_OFFSET(PO_FX_DEVICE_V1, ComponentIdleConditionCallback, 16);
ABI_OFFSET(PO_FX_DEVICE_V1, ComponentIdleStateCallback, 24);
ABI_OFFSET(PO_FX_DEVICE_V1, DevicePowerRequiredCallback, 32);
ABI_OFFSET(PO_FX_DEVICE_V1, DevicePowerNotRequiredCallback, 40);
ABI_OFFSET(PO_FX_DEVICE_V1, PowerControlCallback, 48);
ABI_OFFSET(PO_FX_DEVICE_V1, DeviceContext, 56);
ABI_OFFSET(PO_FX_DEVICE_V1, Components, 64);
ABI_OFFSET(PO_FX_COMPONENT_V1, IdleStateCount, 16);
ABI_OFFSET(PO_FX_COMPONENT_V1, DeepestWakeableIdleState, 20);
ABI_OFFSET(PO_FX_COMPONENT_V1, IdleStates, 24);
ABI_OFFSET(PO_FX_COMPONENT_IDLE_STATE, TransitionLatency, 0);
ABI_OFFSET(PO_FX_COMPONENT_IDLE_STATE, ResidencyRequirement, 8);
ABI_OFFSET(PO_FX_COMPONENT_IDLE_STATE, NominalPower, 16);
_Static_assert(PO_FX_VERSION_V1 == 1 && PO_FX_FLAG_BLOCKING == 1 &&
                   PO_FX_FLAG_ASYNC_ONLY == 2,
               "PoFx version and operation flags");

typedef struct {
  PDEVICE_OBJECT Self;
  PDEVICE_OBJECT PDO;
  PDEVICE_OBJECT Lower;
  PIO_WORKITEM WorkItem;
  KEVENT PnpEvent;
  POHANDLE Handle;
  PETHREAD Caller;
  PIRP Request;
  ULONG ActiveCalls;
  ULONG IdleCalls;
  ULONG FxState;
  BOOLEAN Blocking;
  BOOLEAN DelayIdle;
  BOOLEAN DelayState;
  BOOLEAN IdleCompleted;
} POWER_EXTENSION;

static ULONG LiveDevices;

static void Check(BOOLEAN Condition, const char *Message) {
  if (!Condition)
    DbgPrint("PoFx failure: %s\n", Message);
}

static NTSTATUS Finish(PIRP Irp, NTSTATUS Status) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static VOID CompleteIdleWorker(PDEVICE_OBJECT Device, PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && PsGetCurrentThread() != Extension->Caller,
        "idle acknowledgement must run in the worker thread");
  Extension->IdleCompleted = TRUE;
  PoFxCompleteIdleCondition(Extension->Handle, 0);
  DbgPrint("PoFx delayed idle acknowledged\n");
}

static VOID ActiveCondition(PVOID Context, ULONG Component) {
  POWER_EXTENSION *Extension = Context;
  Check(Component == 0 && Extension->FxState == 0,
        "active callback must observe F0");
  if (Extension->Caller)
    Check((PsGetCurrentThread() == Extension->Caller) == Extension->Blocking,
          "active callback thread does not match the operation flags");
  ++Extension->ActiveCalls;
  DbgPrint("PoFx active callback\n");
  if (Extension->Request) {
    PIRP Request = Extension->Request;
    Extension->Request = NULL;
    PoFxIdleComponent(Extension->Handle, 0, PO_FX_FLAG_ASYNC_ONLY);
    Finish(Request, STATUS_SUCCESS);
  }
}

static VOID IdleCondition(PVOID Context, ULONG Component) {
  POWER_EXTENSION *Extension = Context;
  Check(Component == 0, "idle callback component");
  if (Extension->Caller)
    Check((PsGetCurrentThread() == Extension->Caller) == Extension->Blocking,
          "idle callback thread does not match the operation flags");
  ++Extension->IdleCalls;
  DbgPrint("PoFx idle callback\n");
  if (Extension->DelayIdle) {
    Extension->DelayIdle = FALSE;
    IoQueueWorkItem(Extension->WorkItem, CompleteIdleWorker, DelayedWorkQueue,
                    Extension);
    return;
  }
  Extension->IdleCompleted = TRUE;
  PoFxCompleteIdleCondition(Extension->Handle, Component);
}

static VOID CompleteStateWorker(PDEVICE_OBJECT Device, PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  Check(Device == Extension->Self && PsGetCurrentThread() != Extension->Caller,
        "F0 acknowledgement must run in the worker thread");
  PoFxCompleteIdleState(Extension->Handle, 0);
  DbgPrint("PoFx delayed F0 acknowledged\n");
}

static VOID IdleState(PVOID Context, ULONG Component, ULONG State) {
  POWER_EXTENSION *Extension = Context;
  Check(Component == 0 && State <= 1, "idle-state callback target");
  if (Extension->Blocking && State == 0)
    Check(PsGetCurrentThread() == Extension->Caller,
          "blocking F0 callback must remain on the caller thread");
  Extension->FxState = State;
  if (Extension->DelayState && State == 0) {
    Extension->DelayState = FALSE;
    IoQueueWorkItem(Extension->WorkItem, CompleteStateWorker, DelayedWorkQueue,
                    Extension);
    return;
  }
  PoFxCompleteIdleState(Extension->Handle, Component);
  DbgPrint("PoFx state F%lu\n", State);
}

static VOID DevicePowerRequired(PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  // This resource-free fixture deliberately remains in D0 when idle permission
  // arrives; the report acknowledges the callback without fabricating an IRP.
  PoFxReportDevicePoweredOn(Extension->Handle);
  DbgPrint("PoFx power required acknowledged\n");
}

static VOID DevicePowerNotRequired(PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  PoFxCompleteDevicePowerNotRequired(Extension->Handle);
  DbgPrint("PoFx power not required acknowledged\n");
}

static NTSTATUS RegisterPower(POWER_EXTENSION *Extension) {
  PO_FX_COMPONENT_IDLE_STATE States[2] = {{0, 0, 100}, {10, 20, 50}};
  PO_FX_DEVICE_V1 Device = {0};
  Device.Version = PO_FX_VERSION_V1;
  Device.ComponentCount = 1;
  Device.ComponentActiveConditionCallback = ActiveCondition;
  Device.ComponentIdleConditionCallback = IdleCondition;
  Device.ComponentIdleStateCallback = IdleState;
  Device.DevicePowerRequiredCallback = DevicePowerRequired;
  Device.DevicePowerNotRequiredCallback = DevicePowerNotRequired;
  Device.DeviceContext = Extension;
  Device.Components[0].IdleStateCount = RTL_NUMBER_OF(States);
  Device.Components[0].DeepestWakeableIdleState = 1;
  Device.Components[0].IdleStates = States;
  NTSTATUS Status =
      PoFxRegisterDevice(Extension->PDO, &Device, &Extension->Handle);
  if (!NT_SUCCESS(Status))
    return Status;
  PoFxSetComponentLatency(Extension->Handle, 0, 100);
  PoFxSetComponentResidency(Extension->Handle, 0, 1000);
  PoFxSetComponentWake(Extension->Handle, 0, TRUE);
  PoFxSetDeviceIdleTimeout(Extension->Handle, 0);
  PoFxStartDevicePowerManagement(Extension->Handle);
  DbgPrint("PoFx registered and started\n");
  return STATUS_SUCCESS;
}

static NTSTATUS PnpCompletion(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context) {
  POWER_EXTENSION *Extension = Context;
  UNREFERENCED_PARAMETER(Irp);
  Check(Device == Extension->Self, "PnP completion device");
  KeSetEvent(&Extension->PnpEvent, IO_NO_INCREMENT, FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT Device, PIRP Irp) {
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  const UCHAR Minor = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
  NTSTATUS Status;
  Irp->IoStatus.Status = STATUS_SUCCESS;
  Irp->IoStatus.Information = 0;
  if (Minor == IRP_MN_REMOVE_DEVICE) {
    PDEVICE_OBJECT Lower = Extension->Lower;
    PoFxUnregisterDevice(Extension->Handle);
    Extension->Handle = NULL;
    IoFreeWorkItem(Extension->WorkItem);
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Lower, Irp);
    IoDetachDevice(Lower);
    --LiveDevices;
    IoDeleteDevice(Device);
    DbgPrint("PoFx removed\n");
    return Status;
  }
  KeClearEvent(&Extension->PnpEvent);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp, PnpCompletion, Extension, TRUE, TRUE, TRUE);
  Status = IoCallDriver(Extension->Lower, Irp);
  if (Status == STATUS_PENDING)
    KeWaitForSingleObject(&Extension->PnpEvent, Executive, KernelMode, FALSE,
                          NULL);
  Status = Irp->IoStatus.Status;
  if (Minor == IRP_MN_START_DEVICE && NT_SUCCESS(Status))
    Status = RegisterPower(Extension);
  return Finish(Irp, Status);
}

static NTSTATUS DispatchFile(PDEVICE_OBJECT Device, PIRP Irp) {
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Finish(Irp, STATUS_SUCCESS);
  if (Stack->Parameters.DeviceIoControl.InputBufferLength != 1)
    return Finish(Irp, STATUS_INVALID_PARAMETER);
  const UCHAR Mode = *(const UCHAR *)Irp->AssociatedIrp.SystemBuffer;
  if (Mode != 'A' && Mode != 'B' && Mode != 'D' && Mode != 'F' && Mode != 'G' &&
      Mode != 'P')
    return Finish(Irp, STATUS_INVALID_PARAMETER);
  Extension->Caller = PsGetCurrentThread();
  Extension->Blocking = Mode != 'A';
  Extension->IdleCompleted = FALSE;
  if (Mode == 'A') {
    Extension->Request = Irp;
    IoMarkIrpPending(Irp);
    PoFxActivateComponent(Extension->Handle, 0, PO_FX_FLAG_ASYNC_ONLY);
    return STATUS_PENDING;
  }
  const ULONG ActiveBefore = Extension->ActiveCalls;
  const ULONG IdleBefore = Extension->IdleCalls;
  Extension->DelayState = Mode == 'G';
  if (Mode == 'F' || Mode == 'G' || Mode == 'P') {
    LARGE_INTEGER Delay;
    Delay.QuadPart = -20;
    Check(KeDelayExecutionThread(KernelMode, FALSE, &Delay) == STATUS_SUCCESS,
          "explicit power decision wait");
  }
  PoFxActivateComponent(Extension->Handle, 0, PO_FX_FLAG_BLOCKING);
  Check(Extension->ActiveCalls == ActiveBefore + 1,
        "blocking activation returned before notification");
  Extension->DelayIdle = Mode == 'D';
  PoFxIdleComponent(Extension->Handle, 0, PO_FX_FLAG_BLOCKING);
  Check(Extension->IdleCalls == IdleBefore + 1 && Extension->IdleCompleted,
        "blocking idle returned before completion");
  DbgPrint("PoFx blocking cycle complete mode=%c\n", Mode);
  Extension->Caller = NULL;
  return Finish(Irp, STATUS_SUCCESS);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT Driver, PDEVICE_OBJECT PDO) {
  PDEVICE_OBJECT Device;
  NTSTATUS Status = IoCreateDevice(Driver, sizeof(POWER_EXTENSION), NULL,
                                   FILE_DEVICE_UNKNOWN, 0, FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  POWER_EXTENSION *Extension = Device->DeviceExtension;
  Extension->Self = Device;
  Extension->PDO = PDO;
  Extension->Lower = IoAttachDeviceToDeviceStack(Device, PDO);
  Extension->WorkItem = IoAllocateWorkItem(Device);
  if (!Extension->Lower || !Extension->WorkItem) {
    if (Extension->WorkItem)
      IoFreeWorkItem(Extension->WorkItem);
    if (Extension->Lower)
      IoDetachDevice(Extension->Lower);
    IoDeleteDevice(Device);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  KeInitializeEvent(&Extension->PnpEvent, NotificationEvent, FALSE);
  Device->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  ++LiveDevices;
  return STATUS_SUCCESS;
}

static VOID Unload(PDRIVER_OBJECT Driver) {
  Check(!Driver->DeviceObject && !LiveDevices, "unload has a live device");
  DbgPrint("PoFx unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  UNREFERENCED_PARAMETER(RegistryPath);
  Driver->DriverExtension->AddDevice = AddDevice;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
  Driver->MajorFunction[IRP_MJ_CREATE] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = DispatchFile;
  Driver->MajorFunction[IRP_MJ_CLOSE] = DispatchFile;
  return STATUS_SUCCESS;
}

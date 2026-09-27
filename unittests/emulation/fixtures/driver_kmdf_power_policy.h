//===- driver_kmdf_power_policy.h - Genuine WDK idle/wake callbacks ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "driver_kmdf_power_policy_test.h"

ABI_SLOT(WdfDeviceInitSetPowerPolicyEventCallbacks, 56);
ABI_SLOT(WdfDeviceAssignS0IdleSettings, 46);
ABI_SLOT(WdfDeviceAssignSxWakeSettings, 47);
ABI_SLOT(WdfDeviceStopIdleNoTrack, 88);
ABI_SLOT(WdfDeviceResumeIdleNoTrack, 89);
_Static_assert(sizeof(WDF_POWER_POLICY_EVENT_CALLBACKS) == 64,
               "KMDF power policy callback layout");
_Static_assert(sizeof(WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS) == 36,
               "KMDF idle settings layout");
_Static_assert(sizeof(WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS) == 20,
               "KMDF wake settings layout");

static WCHAR PolicyMode;
static ULONG PolicyEntries, PolicyExits, PolicyArms, PolicyWakes, PolicyDisarms;
static BOOLEAN PolicyInD0, PolicyArmed, PolicyFinalExit;
static volatile ULONG *PolicyRegisters;
static ULONG PolicyRestoredRegister, PolicyRestoredCount;

static BOOLEAN UsesColdPowerPolicy(VOID) {
  return PolicyMode == KmdfPowerColdExplicit ||
         PolicyMode == KmdfPowerColdDefault ||
         PolicyMode == KmdfPowerColdExcluded ||
         PolicyMode == KmdfPowerColdDefaultHot ||
         PolicyMode == KmdfPowerColdNoWake ||
         PolicyMode == KmdfPowerColdWakeUnavailable ||
         PolicyMode == KmdfPowerColdSystemWake;
}
static BOOLEAN ExpectsColdPowerCycle(VOID) {
  return PolicyMode == KmdfPowerColdExplicit ||
         PolicyMode == KmdfPowerColdDefault ||
         PolicyMode == KmdfPowerColdNoWake ||
         PolicyMode == KmdfPowerColdSystemWake;
}
static BOOLEAN UsesPowerPolicy(VOID) {
  return ServiceMode == L'-' &&
         (PolicyMode == KmdfPowerIdleWake || PolicyMode == KmdfPowerIdleOnly ||
          PolicyMode == KmdfPowerSystemWake ||
          PolicyMode == KmdfPowerArmFailure ||
          PolicyMode == KmdfPowerSystemArmFailure ||
          PolicyMode == KmdfPowerManagedQueue ||
          PolicyMode == KmdfPowerManagedWait ||
          PolicyMode == KmdfPowerIdleAndSystemWake ||
          PolicyMode == KmdfPowerRemainIdleOnSystemWake ||
          PolicyMode == KmdfPowerSystemManaged ||
          PolicyMode == KmdfPowerSystemManagedHint || UsesColdPowerPolicy());
}
static NTSTATUS PolicyD0Entry(WDFDEVICE Device,
                              WDF_POWER_DEVICE_STATE Previous) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || PolicyInD0 ||
      Previous != ((!PolicyEntries || PolicyFinalExit) ? WdfPowerDeviceD3Final
                                                       : WdfPowerDeviceD3))
    return STATUS_INVALID_DEVICE_STATE;
  if (UsesColdPowerPolicy()) {
    if (PolicyRegisters == NULL)
      return STATUS_INVALID_DEVICE_STATE;
    PolicyRestoredRegister = READ_REGISTER_ULONG(PolicyRegisters);
    if (PolicyRestoredRegister != ((!PolicyEntries || ExpectsColdPowerCycle())
                                       ? InitialRegisterValue
                                       : KmdfPowerRegisterMarker))
      return STATUS_INVALID_DEVICE_STATE;
    if (PolicyEntries && PolicyRestoredRegister == InitialRegisterValue)
      ++PolicyRestoredCount;
    WRITE_REGISTER_ULONG(PolicyRegisters, KmdfPowerRegisterMarker);
  }
  ++PolicyEntries;
  PolicyInD0 = TRUE;
  return STATUS_SUCCESS;
}
static NTSTATUS PolicyD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || !PolicyInD0 ||
      (Target != WdfPowerDeviceD3 && Target != WdfPowerDeviceD3Final))
    return STATUS_INVALID_DEVICE_STATE;
  ++PolicyExits;
  PolicyFinalExit = Target == WdfPowerDeviceD3Final;
  PolicyInD0 = FALSE;
  return STATUS_SUCCESS;
}
static NTSTATUS PolicyArm(WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || !PolicyInD0 || PolicyArmed)
    return STATUS_INVALID_DEVICE_STATE;
  ++PolicyArms;
  if (PolicyMode == KmdfPowerArmFailure ||
      PolicyMode == KmdfPowerSystemArmFailure)
    return STATUS_UNSUCCESSFUL;
  PolicyArmed = TRUE;
  return STATUS_SUCCESS;
}
static VOID PolicyTriggered(WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || !PolicyArmed || !PolicyInD0)
    ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
  ++PolicyWakes;
}
static VOID PolicyDisarm(WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
      (!PolicyArmed && PolicyMode != KmdfPowerSystemArmFailure))
    ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
  ++PolicyDisarms;
  PolicyArmed = FALSE;
}
static NTSTATUS PolicyPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw,
                                      WDFCMRESLIST Translated) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || PolicyRegisters != NULL ||
      WdfCmResourceListGetCount(Raw) != 1 ||
      WdfCmResourceListGetCount(Translated) != 1)
    return STATUS_INVALID_DEVICE_STATE;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor =
      WdfCmResourceListGetDescriptor(Translated, 0);
  if (Descriptor == NULL || Descriptor->Type != CmResourceTypeMemory ||
      Descriptor->u.Memory.Start.QuadPart != TranslatedMemoryStart ||
      Descriptor->u.Memory.Length != MemoryLength)
    return STATUS_INVALID_DEVICE_STATE;
  PolicyRegisters = MmMapIoSpace(Descriptor->u.Memory.Start,
                                 Descriptor->u.Memory.Length, MmNonCached);
  return PolicyRegisters ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}
static NTSTATUS PolicyReleaseHardware(WDFDEVICE Device,
                                      WDFCMRESLIST Translated) {
  UNREFERENCED_PARAMETER(Device);
  if (KeGetCurrentIrql() != PASSIVE_LEVEL || PolicyRegisters == NULL ||
      WdfCmResourceListGetCount(Translated) != 1)
    return STATUS_INVALID_DEVICE_STATE;
  MmUnmapIoSpace((PVOID)PolicyRegisters, MemoryLength);
  PolicyRegisters = NULL;
  return STATUS_SUCCESS;
}
static VOID PolicyInitialize(PWDFDEVICE_INIT Init) {
  WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
  WDF_POWER_POLICY_EVENT_CALLBACKS Policy;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
  Pnp.EvtDeviceD0Entry = PolicyD0Entry;
  Pnp.EvtDeviceD0Exit = PolicyD0Exit;
  if (UsesColdPowerPolicy()) {
    Pnp.EvtDevicePrepareHardware = PolicyPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = PolicyReleaseHardware;
  }
  WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);
  WDF_POWER_POLICY_EVENT_CALLBACKS_INIT(&Policy);
  Policy.EvtDeviceArmWakeFromS0 = PolicyArm;
  Policy.EvtDeviceDisarmWakeFromS0 = PolicyDisarm;
  Policy.EvtDeviceWakeFromS0Triggered = PolicyTriggered;
  Policy.EvtDeviceArmWakeFromSx = PolicyArm;
  Policy.EvtDeviceDisarmWakeFromSx = PolicyDisarm;
  Policy.EvtDeviceWakeFromSxTriggered = PolicyTriggered;
  WdfDeviceInitSetPowerPolicyEventCallbacks(Init, &Policy);
}
static NTSTATUS PolicyConfigure(WDFDEVICE Device) {
  if (PolicyMode == KmdfPowerSystemWake ||
      PolicyMode == KmdfPowerSystemArmFailure ||
      PolicyMode == KmdfPowerIdleAndSystemWake ||
      PolicyMode == KmdfPowerColdSystemWake) {
    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS Wake;
    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&Wake);
    Wake.DxState = PowerDeviceD3;
    Wake.UserControlOfWakeSettings = WakeDoNotAllowUserControl;
    NTSTATUS Status = WdfDeviceAssignSxWakeSettings(Device, &Wake);
    if (!NT_SUCCESS(Status) || (PolicyMode != KmdfPowerIdleAndSystemWake &&
                                PolicyMode != KmdfPowerColdSystemWake))
      return Status;
  }
  WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS Idle;
  WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(
      &Idle,
      (PolicyMode == KmdfPowerIdleOnly || PolicyMode == KmdfPowerManagedQueue ||
       PolicyMode == KmdfPowerManagedWait ||
       PolicyMode == KmdfPowerRemainIdleOnSystemWake ||
       PolicyMode == KmdfPowerColdNoWake ||
       PolicyMode == KmdfPowerColdSystemWake ||
       PolicyMode == KmdfPowerSystemManaged ||
       PolicyMode == KmdfPowerSystemManagedHint)
          ? IdleCannotWakeFromS0
          : IdleCanWakeFromS0);
  Idle.DxState = PowerDeviceD3;
  Idle.IdleTimeout = KmdfPowerTimeoutMs;
  Idle.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
  Idle.IdleTimeoutType = PolicyMode == KmdfPowerSystemManaged
                             ? SystemManagedIdleTimeout
                         : PolicyMode == KmdfPowerSystemManagedHint
                             ? SystemManagedIdleTimeoutWithHint
                             : DriverManagedIdleTimeout;
  Idle.ExcludeD3Cold = PolicyMode == KmdfPowerColdExplicit ||
                               PolicyMode == KmdfPowerColdNoWake ||
                               PolicyMode == KmdfPowerColdWakeUnavailable ||
                               PolicyMode == KmdfPowerColdSystemWake
                           ? WdfFalse
                       : PolicyMode == KmdfPowerColdDefault ||
                               PolicyMode == KmdfPowerColdDefaultHot
                           ? WdfUseDefault
                           : WdfTrue;
  Idle.PowerUpIdleDeviceOnSystemWake =
      PolicyMode == KmdfPowerRemainIdleOnSystemWake ? WdfFalse : WdfTrue;
  return WdfDeviceAssignS0IdleSettings(Device, &Idle);
}
static VOID PolicyCompleteSnapshot(WDFREQUEST Request, NTSTATUS Status) {
  ULONG *Output;
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  const ULONG WordCount = UsesColdPowerPolicy() ? KmdfPowerColdSnapshotWords
                                                : KmdfPowerSnapshotWords;
  Status = WdfRequestRetrieveOutputBuffer(Request, WordCount * sizeof(*Output),
                                          (PVOID *)&Output, NULL);
  if (NT_SUCCESS(Status)) {
    Output[0] = PolicyEntries;
    Output[1] = PolicyExits;
    Output[2] = PolicyArms;
    Output[3] = PolicyWakes;
    Output[4] = PolicyDisarms;
    Output[5] = PolicyInD0;
    if (UsesColdPowerPolicy()) {
      Output[6] = PolicyRestoredRegister;
      Output[7] = PolicyRestoredCount;
    }
  }
  WdfRequestCompleteWithInformation(
      Request, Status, NT_SUCCESS(Status) ? WordCount * sizeof(*Output) : 0);
}
static struct {
  WDFDEVICE Device;
  WDFREQUEST Request;
  PIO_WORKITEM Item;
} PolicyWait;
static VOID PolicyWaitWorker(PDEVICE_OBJECT DeviceObject, PVOID Context) {
  UNREFERENCED_PARAMETER(Context);
  if (DeviceObject != WdfDeviceWdmGetDeviceObject(PolicyWait.Device) ||
      KeGetCurrentIrql() != PASSIVE_LEVEL || PolicyInD0)
    ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
  DbgPrint("KMDF power: waiting for D0\n");
  NTSTATUS Status = WdfDeviceStopIdle(PolicyWait.Device, TRUE);
  if (NT_SUCCESS(Status)) {
    if (!PolicyInD0)
      ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
    WdfDeviceResumeIdle(PolicyWait.Device);
    DbgPrint("KMDF power: resumed in D0\n");
  }
  PolicyCompleteSnapshot(PolicyWait.Request, Status);
  IoFreeWorkItem(PolicyWait.Item);
  PolicyWait.Item = NULL;
}
static VOID PolicyIoControl(WDFQUEUE Queue, WDFREQUEST Request) {
  UCHAR *Input;
  NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input),
                                                  (PVOID *)&Input, NULL);
  if (!NT_SUCCESS(Status)) {
    WdfRequestComplete(Request, Status);
    return;
  }
  WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
  switch (*Input) {
  case KmdfPowerSnapshot:
    Status = STATUS_SUCCESS;
    break;
  case KmdfPowerReadRegister:
    if (!UsesColdPowerPolicy() || PolicyRegisters == NULL)
      ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
    PolicyRestoredRegister = READ_REGISTER_ULONG(PolicyRegisters);
    Status = STATUS_SUCCESS;
    break;
  case KmdfPowerHold:
    Status = WdfDeviceStopIdle(Device, FALSE);
    break;
  case KmdfPowerRelease:
    WdfDeviceResumeIdle(Device);
    Status = STATUS_SUCCESS;
    break;
  case KmdfPowerWaitD0:
    Status = WdfDeviceStopIdle(Device, TRUE);
    if (NT_SUCCESS(Status))
      WdfDeviceResumeIdle(Device);
    break;
  case KmdfPowerWaitD0Worker: {
    LARGE_INTEGER Delay;
    if (PolicyWait.Item)
      ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
    PolicyWait.Device = Device;
    PolicyWait.Request = Request;
    PolicyWait.Item = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(Device));
    if (!PolicyWait.Item) {
      WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
      return;
    }
    IoQueueWorkItem(PolicyWait.Item, PolicyWaitWorker, DelayedWorkQueue, NULL);
    // Let the actual worker enter StopIdle before this dispatch returns. The
    // following explicit system power packet is responsible for waking it.
    Delay.QuadPart = -1;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    return;
  }
  default:
    Status = STATUS_INVALID_PARAMETER;
    break;
  }
  PolicyCompleteSnapshot(Request, Status);
}

//===- driver_kmdf_locks.c - Genuine WDK lock ownership fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// Original KMDF code linked through the genuine WDK entry library. Two real
/// threads contend for a wait lock while the owner waits on an independent
/// dispatcher event. Timeout and acquisition must preserve each thread's APC
/// state; spin acquisition must restore the caller's original IRQL.
//===----------------------------------------------------------------------===//

#include <ntifs.h>
#include <wdf.h>

#define ABI_SLOT(Name, Index)                                                  \
  _Static_assert(Name##TableIndex == Index, #Name " table slot")
ABI_SLOT(WdfWaitLockCreate, 312);
ABI_SLOT(WdfWaitLockAcquire, 313);
ABI_SLOT(WdfWaitLockRelease, 314);
ABI_SLOT(WdfSpinLockCreate, 315);
ABI_SLOT(WdfSpinLockAcquire, 316);
ABI_SLOT(WdfSpinLockRelease, 317);

static WDFDEVICE Device;
static WDFWAITLOCK WaitLock;
static WDFSPINLOCK SpinLock;
static PIO_WORKITEM WorkItem;
static KEVENT Contended;
static KEVENT Completed;
static KEVENT ReleaseOwner;
static BOOLEAN Failed;

static BOOLEAN Check(BOOLEAN Condition, PCSTR Description) {
  if (!Condition) {
    Failed = TRUE;
    DbgPrint("KMDF locks: failure %s\n", Description);
  }
  return Condition;
}

static void Worker(PDEVICE_OBJECT DeviceObject, PVOID Context) {
  LARGE_INTEGER Timeout;
  UNREFERENCED_PARAMETER(Context);
  Check(DeviceObject == WdfDeviceWdmGetDeviceObject(Device), "worker device");
  Check(KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreApcsDisabled(),
        "worker entry state");
  Timeout.QuadPart = 0;
  Check(WdfWaitLockAcquire(WaitLock, &Timeout.QuadPart) == STATUS_TIMEOUT &&
            !KeAreApcsDisabled(),
        "zero timeout");
  Timeout.QuadPart = -10;
  Check(WdfWaitLockAcquire(WaitLock, &Timeout.QuadPart) == STATUS_TIMEOUT &&
            !KeAreApcsDisabled(),
        "relative timeout");
  KeSetEvent(&Contended, IO_NO_INCREMENT, FALSE);
  Check(WdfWaitLockAcquire(WaitLock, NULL) == STATUS_SUCCESS &&
            KeAreApcsDisabled() && KeGetCurrentIrql() == PASSIVE_LEVEL,
        "contended acquisition");
  WdfWaitLockRelease(WaitLock);
  Check(!KeAreApcsDisabled(), "worker release");
  DbgPrint("KMDF locks: worker completed\n");
  KeSetEvent(&Completed, IO_NO_INCREMENT, FALSE);
}

static void CallbackLockWorker(PDEVICE_OBJECT DeviceObject, PVOID Context) {
  KIRQL Previous;
  const BOOLEAN Owner = Context != NULL;
  Check(DeviceObject == WdfDeviceWdmGetDeviceObject(Device),
        "callback worker device");
  KeRaiseIrql(APC_LEVEL, &Previous);
  if (!Owner)
    KeSetEvent(&Contended, IO_NO_INCREMENT, FALSE);
  WdfObjectAcquireLock(Device);
  Check(KeGetCurrentIrql() == APC_LEVEL && KeAreApcsDisabled(),
        "APC callback acquisition");
  if (Owner) {
    KeSetEvent(&Contended, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(&ReleaseOwner, Executive, KernelMode, FALSE, NULL);
    Check(KeGetCurrentIrql() == APC_LEVEL && KeAreApcsDisabled(),
          "APC owner event resume");
  }
  WdfObjectReleaseLock(Device);
  Check(KeGetCurrentIrql() == APC_LEVEL && !KeAreApcsDisabled(),
        "APC callback release");
  KeLowerIrql(Previous);
  DbgPrint(Owner ? "KMDF locks: APC owner completed\n"
                 : "KMDF locks: APC waiter completed\n");
  KeSetEvent(&Completed, IO_NO_INCREMENT, FALSE);
}

static void DriverUnload(WDFDRIVER Driver) {
  UNREFERENCED_PARAMETER(Driver);
  WdfObjectDelete(Device);
  DbgPrint("KMDF locks: unload\n");
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDFDRIVER Driver;
  PWDFDEVICE_INIT DeviceInit;
  UNICODE_STRING Security, Name;
  NTSTATUS Status;
  KIRQL OriginalIRQL;
  WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
  Config.DriverInitFlags = WdfDriverInitNonPnpDriver;
  Config.EvtDriverUnload = DriverUnload;
  Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                           &Config, &Driver);
  if (!NT_SUCCESS(Status))
    return Status;
  RtlInitUnicodeString(&Security, L"D:P(A;;GA;;;WD)");
  DeviceInit = WdfControlDeviceInitAllocate(Driver, &Security);
  if (!DeviceInit)
    return STATUS_INSUFFICIENT_RESOURCES;
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDKmdfLocks");
  Status = WdfDeviceInitAssignName(DeviceInit, &Name);
  if (!NT_SUCCESS(Status)) {
    WdfDeviceInitFree(DeviceInit);
    return Status;
  }
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ExecutionLevel = WdfExecutionLevelPassive;
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  WdfControlFinishInitializing(Device);
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ParentObject = Device;
  Status = WdfSpinLockCreate(&Attributes, &SpinLock);
  if (!NT_SUCCESS(Status))
    return Status;
  Status = WdfWaitLockCreate(&Attributes, &WaitLock);
  if (!NT_SUCCESS(Status))
    return Status;
  OriginalIRQL = KeGetCurrentIrql();
  WdfSpinLockAcquire(SpinLock);
  Check(KeGetCurrentIrql() == DISPATCH_LEVEL, "spin acquire IRQL");
  WdfSpinLockRelease(SpinLock);
  Check(KeGetCurrentIrql() == OriginalIRQL, "spin release IRQL");
  Check(WdfWaitLockAcquire(WaitLock, NULL) == STATUS_SUCCESS &&
            KeAreApcsDisabled(),
        "owner acquisition");
  KeInitializeEvent(&Contended, NotificationEvent, FALSE);
  KeInitializeEvent(&Completed, NotificationEvent, FALSE);
  WorkItem = IoAllocateWorkItem(WdfDeviceWdmGetDeviceObject(Device));
  if (!WorkItem)
    return STATUS_INSUFFICIENT_RESOURCES;
  IoQueueWorkItem(WorkItem, Worker, DelayedWorkQueue, NULL);
  KeWaitForSingleObject(&Contended, Executive, KernelMode, FALSE, NULL);
  Check(KeAreApcsDisabled(), "owner remains in critical region");
  WdfWaitLockRelease(WaitLock);
  Check(!KeAreApcsDisabled(), "owner release");
  KeWaitForSingleObject(&Completed, Executive, KernelMode, FALSE, NULL);
  KeClearEvent(&Contended);
  KeClearEvent(&Completed);
  WdfObjectAcquireLock(Device);
  IoQueueWorkItem(WorkItem, CallbackLockWorker, DelayedWorkQueue, NULL);
  KeWaitForSingleObject(&Contended, Executive, KernelMode, FALSE, NULL);
  WdfObjectReleaseLock(Device);
  KeWaitForSingleObject(&Completed, Executive, KernelMode, FALSE, NULL);

  KeClearEvent(&Contended);
  KeClearEvent(&Completed);
  KeInitializeEvent(&ReleaseOwner, NotificationEvent, FALSE);
  IoQueueWorkItem(WorkItem, CallbackLockWorker, DelayedWorkQueue, Device);
  KeWaitForSingleObject(&Contended, Executive, KernelMode, FALSE, NULL);
  KeRaiseIrql(APC_LEVEL, &OriginalIRQL);
  KeSetEvent(&ReleaseOwner, IO_NO_INCREMENT, FALSE);
  WdfObjectAcquireLock(Device);
  Check(KeGetCurrentIrql() == APC_LEVEL && KeAreApcsDisabled(),
        "foreground APC callback acquisition");
  WdfObjectReleaseLock(Device);
  Check(KeGetCurrentIrql() == APC_LEVEL && !KeAreApcsDisabled(),
        "foreground APC callback release");
  KeLowerIrql(OriginalIRQL);
  KeWaitForSingleObject(&Completed, Executive, KernelMode, FALSE, NULL);
  IoFreeWorkItem(WorkItem);
  if (Failed)
    return STATUS_UNSUCCESSFUL;
  DbgPrint("KMDF locks: complete\n");
  return STATUS_SUCCESS;
}

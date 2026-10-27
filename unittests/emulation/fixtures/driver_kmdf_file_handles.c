//===- driver_kmdf_file_handles.c - Original KMDF file handles -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Genuine WDK callbacks distinguish the handle table's process from the
/// closing thread's creating process. Mode N omits EvtFileCleanup; mode F
/// releases the retained handle from another file's scenario cleanup callback.

#include <ntifs.h>
#include <wdf.h>

static WDFDEVICE Device;
static PFILE_OBJECT Old;
static HANDLE Handle;
static PETHREAD Issuer;
static ULONG Creates, Controls, Cleanups, Closes, Failure;
static ULONG ObjectCleanups, ObjectDestroys;
static WCHAR Mode;

static VOID FileCreate(WDFDEVICE D, WDFREQUEST R, WDFFILEOBJECT F) {
  UNREFERENCED_PARAMETER(D);
  if (++Creates == 1) {
    Old = WdfFileObjectWdmGetFileObject(F);
    ObReferenceObject(Old);
  }
  WdfRequestComplete(R, STATUS_SUCCESS);
}

static VOID FileCleanup(WDFFILEOBJECT F) {
  BOOLEAN Retained = WdfFileObjectWdmGetFileObject(F) == Old;
  if (PsGetCurrentThread() != Issuer ||
      (ULONG_PTR)PsGetCurrentProcessId() != 0x2000 ||
      (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) !=
          (Retained ? 4 : 0x2000) ||
      ExGetPreviousMode() != UserMode)
    Failure = 1;
  if (!Retained && Mode == L'F') {
    LARGE_INTEGER Delay;
    Delay.QuadPart = -10;
    if (!NT_SUCCESS(KeDelayExecutionThread(KernelMode, FALSE, &Delay)) ||
        (ULONG_PTR)PsGetCurrentProcessId() != 0x2000 ||
        (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) != 0x2000 ||
        PsGetCurrentThread() != Issuer || ExGetPreviousMode() != UserMode)
      Failure = 1;
    if (!NT_SUCCESS(ZwClose(Handle)) || Closes != 1 || Cleanups != 1 ||
        (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) != 0x2000 ||
        PsGetCurrentThread() != Issuer || ExGetPreviousMode() != UserMode)
      Failure = 1;
  }
  ++Cleanups;
}

static VOID FileClose(WDFFILEOBJECT F) {
  UNREFERENCED_PARAMETER(F);
  if (PsGetCurrentThread() != Issuer ||
      (ULONG_PTR)PsGetCurrentProcessId() != 0x2000 ||
      (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) != 0x2000 ||
      ExGetPreviousMode() != UserMode)
    Failure = 1;
  ++Closes;
}

static VOID FileObjectCleanup(WDFOBJECT Object) {
  UNREFERENCED_PARAMETER(Object);
  if (PsGetCurrentThread() != Issuer ||
      (ULONG_PTR)PsGetCurrentProcessId() != 0x2000 ||
      (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) != 0x2000 ||
      ExGetPreviousMode() != UserMode)
    Failure = 1;
  ++ObjectCleanups;
}

static VOID FileObjectDestroy(WDFOBJECT Object) {
  UNREFERENCED_PARAMETER(Object);
  if (PsGetCurrentThread() != Issuer ||
      (ULONG_PTR)PsGetCurrentProcessId() != 0x2000 ||
      (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) != 0x2000 ||
      ExGetPreviousMode() != UserMode)
    Failure = 1;
  ++ObjectDestroys;
}

static VOID Control(WDFQUEUE Q, WDFREQUEST R, size_t O, size_t I, ULONG C) {
  UNREFERENCED_PARAMETER(Q);
  UNREFERENCED_PARAMETER(O);
  UNREFERENCED_PARAMETER(I);
  UNREFERENCED_PARAMETER(C);
  NTSTATUS Status;
  if (++Controls == 1) {
    Status = ObOpenObjectByPointer(Old, OBJ_KERNEL_HANDLE, NULL, GENERIC_READ,
                                   *IoFileObjectType, KernelMode, &Handle);
    ObDereferenceObject(Old);
  } else {
    Issuer = PsGetCurrentThread();
    Status = Mode == L'F' ? STATUS_SUCCESS : ZwClose(Handle);
    if (Closes != (Mode == L'F' ? 0u : 1u) ||
        Cleanups != (Mode == L'N' || Mode == L'F' ? 0u : 1u) || Failure ||
        (ULONG_PTR)PsGetProcessId(IoGetCurrentProcess()) != 0x2000 ||
        PsGetCurrentThread() != Issuer || ExGetPreviousMode() != UserMode)
      Status = STATUS_UNSUCCESSFUL;
  }
  WdfRequestComplete(R, Status);
}

static VOID Unload(WDFDRIVER D) {
  UNREFERENCED_PARAMETER(D);
  if (!Failure && Creates == 2 && Controls == 2 && Closes == 2 &&
      Cleanups == (Mode == L'N' ? 0u : 2u) && ObjectCleanups == 2 &&
      ObjectDestroys == 2)
    WdfObjectDelete(Device);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Registry) {
  if (Registry->Length >= sizeof(WCHAR))
    Mode = Registry->Buffer[Registry->Length / sizeof(WCHAR) - 1];
  WDF_DRIVER_CONFIG Config;
  WDF_DRIVER_CONFIG_INIT(&Config, WDF_NO_EVENT_CALLBACK);
  Config.DriverInitFlags = WdfDriverInitNonPnpDriver;
  Config.EvtDriverUnload = Unload;
  WDFDRIVER DriverHandle;
  NTSTATUS Status = WdfDriverCreate(Driver, Registry, WDF_NO_OBJECT_ATTRIBUTES,
                                    &Config, &DriverHandle);
  if (!NT_SUCCESS(Status))
    return Status;
  DECLARE_CONST_UNICODE_STRING(Security, L"D:P(A;;GA;;;WD)");
  PWDFDEVICE_INIT Init = WdfControlDeviceInitAllocate(DriverHandle, &Security);
  if (!Init)
    return STATUS_INSUFFICIENT_RESOURCES;
  DECLARE_CONST_UNICODE_STRING(Name, L"\\Device\\OriginalFileHandles");
  Status = WdfDeviceInitAssignName(Init, &Name);
  if (!NT_SUCCESS(Status))
    return Status;
  WDF_FILEOBJECT_CONFIG Files;
  WDF_FILEOBJECT_CONFIG_INIT(&Files, FileCreate, FileClose,
                             Mode == L'N' ? WDF_NO_EVENT_CALLBACK
                                          : FileCleanup);
  WDF_OBJECT_ATTRIBUTES FileAttributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&FileAttributes);
  FileAttributes.EvtCleanupCallback = FileObjectCleanup;
  FileAttributes.EvtDestroyCallback = FileObjectDestroy;
  WdfDeviceInitSetFileObjectConfig(Init, &Files, &FileAttributes);
  Status = WdfDeviceCreate(&Init, WDF_NO_OBJECT_ATTRIBUTES, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  WDF_IO_QUEUE_CONFIG Queue;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Queue, WdfIoQueueDispatchParallel);
  Queue.EvtIoDeviceControl = Control;
  Status =
      WdfIoQueueCreate(Device, &Queue, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
  if (!NT_SUCCESS(Status))
    return Status;
  WdfControlFinishInitializing(Device);
  return STATUS_SUCCESS;
}

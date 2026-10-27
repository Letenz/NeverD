//===- driver_wdm_user_mapping.c - Genuine WDK process MDL views ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "driver_user_mapping.h"

#include <ntifs.h>

static PDEVICE_OBJECT Device;

typedef struct UserMappingWork {
  PIRP Irp;
  PIO_WORKITEM Work;
  PEPROCESS Process;
  PMDL PoolMdl;
  PMDL ReportMdl;
  PUCHAR Pool;
  PUCHAR Report;
  PUCHAR UserReport;
  PUCHAR UserView;
  ULONG Action;
} UserMappingWork;

static UserMappingWork Pending;

typedef struct UserMappingProcessView {
  PMDL Mdl;
  PUCHAR Pool;
  PUCHAR Address;
  PMDL LockedMdl;
  PUCHAR System;
} UserMappingProcessView;

static UserMappingProcessView Retained[2];

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status) {
  Irp->IoStatus.Status = Status;
  Irp->IoStatus.Information = 0;
  IoCompleteRequest(Irp, IO_NO_INCREMENT);
  return Status;
}

static VOID ReleasePool(PMDL Mdl, PVOID Pool) {
  if (Mdl)
    IoFreeMdl(Mdl);
  ExFreePoolWithTag(Pool, UserMappingPoolTag);
}

static NTSTATUS CheckProcessLocks(UserMappingWork *Work) {
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, L"MmProbeAndLockProcessPages");
  VOID(NTAPI * ProbeProcess)(PMDL, PEPROCESS, KPROCESSOR_MODE, LOCK_OPERATION) =
      (VOID(NTAPI *)(PMDL, PEPROCESS, KPROCESSOR_MODE,
                     LOCK_OPERATION))MmGetSystemRoutineAddress(&Name);
  if (!ProbeProcess)
    return STATUS_PROCEDURE_NOT_FOUND;
  PEPROCESS OriginalProcess = IoGetCurrentProcess();
  PETHREAD Thread = PsGetCurrentThread();
  PMDL Probe = IoAllocateMdl(Work->UserView + UserMappingByteOffset, 1, FALSE,
                             FALSE, NULL);
  if (!Probe)
    return STATUS_INSUFFICIENT_RESOURCES;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    __try {
      // Every nonzero operation is normalized to write access. This view is
      // read-only, so the wrapper must detach before our handler observes it.
      MmProbeAndLockProcessPages(Probe, Work->Process, UserMode,
                                 (LOCK_OPERATION)3);
      Locked = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Work->Report[0] |= 1;
      if (IoGetCurrentProcess() == OriginalProcess &&
          PsGetCurrentThread() == Thread && ExGetPreviousMode() == KernelMode)
        Work->Report[0] |= 2;
    }
    if (Locked || Work->Report[0] != 3)
      ExRaiseStatus(STATUS_INVALID_DEVICE_STATE);
    ProbeProcess(Probe, Work->Process, UserMode, IoReadAccess);
    Locked = TRUE;
    PUCHAR System = MmGetSystemAddressForMdlSafe(Probe, NormalPagePriority);
    Work->Report[1] = System[0];
    if (MmGetMdlPfnArray(Probe)[0] == MmGetMdlPfnArray(Work->PoolMdl)[0])
      Work->Report[3] |= 1;
    if (IoGetCurrentProcess() == OriginalProcess &&
        IoThreadToProcess(Thread) == OriginalProcess &&
        ExGetPreviousMode() == KernelMode)
      Work->Report[3] |= 2;
    MmUnlockPages(Probe);
    Locked = FALSE;

    KAPC_STATE Attachment;
    KeStackAttachProcess(Work->Process, &Attachment);
    __try {
      MmProbeAndLockProcessPages(Probe, Work->Process, UserMode, IoReadAccess);
      Locked = TRUE;
      MmUnmapLockedPages(Work->UserView, Work->PoolMdl);
      Work->UserView = NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Status = GetExceptionCode();
    }
    KeUnstackDetachProcess(&Attachment);
    if (!NT_SUCCESS(Status))
      ExRaiseStatus(Status);
    System = MmGetSystemAddressForMdlSafe(Probe, NormalPagePriority);
    if (System[0] == UserMappingFirstByte &&
        IoGetCurrentProcess() == OriginalProcess)
      Work->Report[3] |= 4;
    MmUnlockPages(Probe);
    Locked = FALSE;
    IoFreeMdl(Probe);
    Probe = IoAllocateMdl(Work->UserReport, UserMappingReportSize, FALSE, FALSE,
                          NULL);
    if (!Probe)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    ProbeProcess(Probe, Work->Process, UserMode, (LOCK_OPERATION)3);
    Locked = TRUE;
    System = MmGetSystemAddressForMdlSafe(Probe, NormalPagePriority);
    System[2] = UserMappingSecondByte;
    if (IoGetCurrentProcess() == OriginalProcess &&
        PsGetCurrentThread() == Thread &&
        IoThreadToProcess(Thread) == OriginalProcess &&
        ExGetPreviousMode() == KernelMode)
      Work->Report[3] |= 8;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (Locked)
    MmUnlockPages(Probe);
  if (Probe)
    IoFreeMdl(Probe);
  if (Work->UserView) {
    KAPC_STATE Attachment;
    KeStackAttachProcess(Work->Process, &Attachment);
    MmUnmapLockedPages(Work->UserView, Work->PoolMdl);
    KeUnstackDetachProcess(&Attachment);
    Work->UserView = NULL;
  }
  return Status;
}

static VOID MappingWorker(PDEVICE_OBJECT Object, PVOID Context) {
  UserMappingWork *Work = Context;
  PIRP Irp = Work->Irp;
  NTSTATUS Status = STATUS_SUCCESS;
  if (Object != Device || Work != &Pending || !Irp)
    Status = STATUS_INVALID_DEVICE_STATE;
  else if (Work->Action == UserMappingWrongProcessUnmap)
    MmUnmapLockedPages(Work->UserView, Work->PoolMdl);
  else if (Work->Action == UserMappingProcessLock)
    Status = CheckProcessLocks(Work);
  else if (Work->Action == UserMappingAttachedWorker) {
    KAPC_STATE Attachment;
    KeStackAttachProcess(Work->Process, &Attachment);
    __try {
      PUCHAR View = MmMapLockedPages(Work->PoolMdl, UserMode);
      View[UserMappingByteOffset] = UserMappingWorkerByte;
      Work->Report[0] = Work->Pool[UserMappingByteOffset];
      Work->Report[1] = MmGetSystemAddressForMdlSafe(
                            Work->PoolMdl, NormalPagePriority) == Work->Pool;
      MmUnmapLockedPages(View, Work->PoolMdl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      Status = GetExceptionCode();
    }
    KeUnstackDetachProcess(&Attachment);
  } else {
    // Process exit removes its user views; the report's independent system
    // mapping and the nonpaged pool remain available to this worker.
    Work->Pool[UserMappingByteOffset] = UserMappingWorkerByte;
    Work->Report[0] = Work->Pool[UserMappingByteOffset];
    Work->Report[1] = MmGetSystemAddressForMdlSafe(
                          Work->PoolMdl, NormalPagePriority) == Work->Pool;
  }
  ReleasePool(Work->PoolMdl, Work->Pool);
  MmUnlockPages(Work->ReportMdl);
  IoFreeMdl(Work->ReportMdl);
  IoFreeWorkItem(Work->Work);
  RtlZeroMemory(Work, sizeof(*Work));
  Complete(Irp, Status);
}

static NTSTATUS CheckPhysicalAddresses(PMDL Mdl, PUCHAR Pool,
                                       volatile UCHAR *Report,
                                       BOOLEAN PrimaryQuery) {
  UNICODE_STRING Routine;
  RtlInitUnicodeString(&Routine, L"MmGetPhysicalAddress");
  PHYSICAL_ADDRESS(NTAPI * Query)(PVOID) =
      (PHYSICAL_ADDRESS(NTAPI *)(PVOID))MmGetSystemRoutineAddress(&Routine);
  if (!Query)
    return STATUS_PROCEDURE_NOT_FOUND;
  PVOID(NTAPI * Reverse)(PHYSICAL_ADDRESS) = NULL;
  if (PrimaryQuery) {
    RtlInitUnicodeString(&Routine, L"MmGetVirtualForPhysical");
    Reverse =
        (PVOID(NTAPI *)(PHYSICAL_ADDRESS))MmGetSystemRoutineAddress(&Routine);
    if (!Reverse)
      return STATUS_PROCEDURE_NOT_FOUND;
  }
  PUCHAR Other =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, UserMappingPoolTag);
  if (!Other)
    return STATUS_INSUFFICIENT_RESOURCES;
  PMDL OtherMdl = IoAllocateMdl(Other, PAGE_SIZE, FALSE, FALSE, NULL);
  if (!OtherMdl) {
    ExFreePoolWithTag(Other, UserMappingPoolTag);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  MmBuildMdlForNonPagedPool(OtherMdl);
  PMDL LockedMdl = NULL, ViewMdl = Mdl;
  PUCHAR View = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    const ULONGLONG Expected =
        ((ULONGLONG)MmGetMdlPfnArray(Mdl)[0] << PAGE_SHIFT) +
        UserMappingByteOffset;
    if ((ULONGLONG)MmGetPhysicalAddress(Pool + UserMappingByteOffset)
            .QuadPart == Expected)
      Report[0] |= 1;
    PHYSICAL_ADDRESS PrimaryAddress;
    PrimaryAddress.QuadPart = Expected;
    if (PrimaryQuery &&
        MmGetVirtualForPhysical(PrimaryAddress) == Pool + UserMappingByteOffset)
      Report[0] |= 4;
    View = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL, FALSE,
                                        NormalPagePriority | MdlMappingNoWrite);
    if ((ULONGLONG)Query(View + UserMappingByteOffset).QuadPart == Expected)
      Report[0] |= 2;
    LockedMdl =
        IoAllocateMdl(View + UserMappingByteOffset, 1, FALSE, FALSE, NULL);
    if (!LockedMdl)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(LockedMdl, UserMode, IoReadAccess);
    Locked = TRUE;
    PUCHAR System = MmGetSystemAddressForMdlSafe(LockedMdl, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    PUCHAR Requested = View;
    MmUnmapLockedPages(View, Mdl);
    View = NULL;
    KIRQL Old;
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
    const PHYSICAL_ADDRESS SystemAddress = MmGetPhysicalAddress(System);
    const PHYSICAL_ADDRESS PoolAddress = Query(Pool + UserMappingByteOffset);
    PVOID SystemPrimary = PrimaryQuery ? Reverse(SystemAddress) : NULL;
    KeLowerIrql(Old);
    // Report is user memory and is only written after restoring APC admission.
    if ((ULONGLONG)SystemAddress.QuadPart == Expected)
      Report[1] |= 1;
    if ((ULONGLONG)PoolAddress.QuadPart == Expected)
      Report[1] |= 2;
    if (PrimaryQuery && SystemPrimary == Pool + UserMappingByteOffset)
      Report[1] |= 4;
    ViewMdl = OtherMdl;
    View = MmMapLockedPagesSpecifyCache(OtherMdl, UserMode, MmCached, Requested,
                                        FALSE,
                                        NormalPagePriority | MdlMappingNoWrite);
    if (View == Requested)
      Report[2] |= 1;
    const ULONGLONG Replacement =
        ((ULONGLONG)MmGetMdlPfnArray(OtherMdl)[0] << PAGE_SHIFT) +
        UserMappingByteOffset;
    if (Replacement != Expected &&
        (ULONGLONG)Query(View + UserMappingByteOffset).QuadPart == Replacement)
      Report[2] |= 2;
    PHYSICAL_ADDRESS ReplacementAddress;
    ReplacementAddress.QuadPart = Replacement;
    if (PrimaryQuery &&
        Reverse(ReplacementAddress) == Other + UserMappingByteOffset)
      Report[2] |= 4;
    if ((ULONGLONG)MmGetPhysicalAddress(System).QuadPart == Expected)
      Report[3] |= 1;
    if (MmGetMdlPfnArray(LockedMdl)[0] == MmGetMdlPfnArray(Mdl)[0] &&
        MmGetMdlPfnArray(LockedMdl)[0] != MmGetMdlPfnArray(OtherMdl)[0])
      Report[3] |= 2;
    if (PrimaryQuery &&
        MmGetVirtualForPhysical(PrimaryAddress) == Pool + UserMappingByteOffset)
      Report[3] |= 4;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (View)
    MmUnmapLockedPages(View, ViewMdl);
  if (Locked)
    MmUnlockPages(LockedMdl);
  if (LockedMdl)
    IoFreeMdl(LockedMdl);
  ReleasePool(OtherMdl, Other);
  return Status;
}

static NTSTATUS CheckAliases(PMDL Mdl, PUCHAR Pool, volatile UCHAR *Report) {
  UNICODE_STRING Routine;
  RtlInitUnicodeString(&Routine, L"MmMapLockedPages");
  PVOID(NTAPI * MapLegacy)(PMDL, KPROCESSOR_MODE) = (PVOID(NTAPI *)(
      PMDL, KPROCESSOR_MODE))MmGetSystemRoutineAddress(&Routine);
  if (!MapLegacy)
    return STATUS_PROCEDURE_NOT_FOUND;
  PUCHAR First = NULL, ReadOnly = NULL;
  PMDL Relocked = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    First = MmMapLockedPages(Mdl, UserMode);
    ReadOnly =
        MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL, TRUE,
                                     NormalPagePriority | MdlMappingNoWrite);
    Pool[UserMappingByteOffset] = UserMappingFirstByte;
    Report[0] = First[UserMappingByteOffset];
    First[UserMappingByteOffset] = UserMappingSecondByte;
    Report[1] = ReadOnly[UserMappingByteOffset];
    Report[2] = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority) == Pool;
    __try {
      ((volatile UCHAR *)ReadOnly)[UserMappingByteOffset] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Report[3] |= 1;
    }
    Relocked =
        IoAllocateMdl(First + UserMappingByteOffset, 1, FALSE, FALSE, NULL);
    if (!Relocked)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(Relocked, UserMode, IoWriteAccess);
    Locked = TRUE;
    if (MmGetMdlPfnArray(Relocked)[0] == MmGetMdlPfnArray(Mdl)[0])
      Report[3] |= 2;
    MmUnmapLockedPages(First, Mdl);
    First = NULL;
    PUCHAR System = MapLegacy(Relocked, KernelMode);
    if (System && System[0] == UserMappingSecondByte)
      Report[3] |= 4;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (Locked)
    MmUnlockPages(Relocked);
  if (Relocked)
    IoFreeMdl(Relocked);
  if (ReadOnly)
    MmUnmapLockedPages(ReadOnly, Mdl);
  if (First)
    MmUnmapLockedPages(First, Mdl);
  return Status;
}

static NTSTATUS CheckAddressReuse(PMDL Mdl, PUCHAR Pool,
                                  volatile UCHAR *Report) {
  PUCHAR OtherPool =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, UserMappingPoolTag);
  if (!OtherPool)
    return STATUS_INSUFFICIENT_RESOURCES;
  PMDL OtherMdl = IoAllocateMdl(OtherPool, PAGE_SIZE, FALSE, FALSE, NULL);
  if (!OtherMdl) {
    ExFreePoolWithTag(OtherPool, UserMappingPoolTag);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  MmBuildMdlForNonPagedPool(OtherMdl);
  PMDL LockedMdl = NULL, ViewMdl = NULL;
  PUCHAR View = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    Pool[UserMappingByteOffset] = UserMappingFirstByte;
    OtherPool[UserMappingByteOffset] = UserMappingSecondByte;
    View = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmNonCached, NULL, FALSE,
                                        NormalPagePriority);
    ViewMdl = Mdl;
    PUCHAR Requested = View;
    LockedMdl =
        IoAllocateMdl(View + UserMappingByteOffset, 1, FALSE, FALSE, NULL);
    if (!LockedMdl)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(LockedMdl, UserMode, IoWriteAccess);
    Locked = TRUE;
    PUCHAR System =
        MmMapLockedPagesSpecifyCache(LockedMdl, KernelMode, MmWriteCombined,
                                     NULL, FALSE, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    const PFN_NUMBER FirstPFN = MmGetMdlPfnArray(LockedMdl)[0];
    MmUnmapLockedPages(View, ViewMdl);
    View = NULL;
    __try {
      Report[0] = ((volatile UCHAR *)Requested)[UserMappingByteOffset];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Report[2] |= UserRemappingUnmappedFault;
    }
    View = MmMapLockedPagesSpecifyCache(OtherMdl, UserMode, MmWriteCombined,
                                        Requested, FALSE,
                                        NormalPagePriority | MdlMappingNoWrite);
    ViewMdl = OtherMdl;
    if (View == Requested)
      Report[2] |= UserRemappingReusedAddress;
    Report[1] = ((volatile UCHAR *)Requested)[UserMappingByteOffset];
    __try {
      ((volatile UCHAR *)Requested)[UserMappingByteOffset] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
        Report[2] |= UserRemappingReadOnlyFault;
    }
    Report[0] = System[0];
    if (FirstPFN == MmGetMdlPfnArray(LockedMdl)[0] &&
        FirstPFN == MmGetMdlPfnArray(Mdl)[0] &&
        FirstPFN != MmGetMdlPfnArray(OtherMdl)[0])
      Report[2] |= UserRemappingPreservedLockedPages;
    MmUnmapLockedPages(View, ViewMdl);
    View = NULL;
    View = MmMapLockedPagesSpecifyCache(OtherMdl, UserMode, MmCached, Requested,
                                        FALSE, NormalPagePriority);
    View[UserMappingByteOffset] = UserMappingWorkerByte;
    Report[3] = View == Requested &&
                OtherPool[UserMappingByteOffset] == UserMappingWorkerByte &&
                System[0] == UserMappingFirstByte;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (View)
    MmUnmapLockedPages(View, ViewMdl);
  if (Locked)
    MmUnlockPages(LockedMdl);
  if (LockedMdl)
    IoFreeMdl(LockedMdl);
  ReleasePool(OtherMdl, OtherPool);
  return Status;
}

static NTSTATUS RetainProcessView(PMDL Mdl, PUCHAR Pool, volatile UCHAR *Report,
                                  BOOLEAN Second) {
  UserMappingProcessView *State = &Retained[Second ? 1 : 0];
  if (State->Mdl || (Second && !Retained[0].Mdl))
    return STATUS_INVALID_DEVICE_STATE;
  PMDL LockedMdl = NULL;
  PUCHAR View = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    Pool[UserMappingByteOffset] =
        Second ? UserMappingSecondByte : UserMappingFirstByte;
    View = MmMapLockedPagesSpecifyCache(
        Mdl, UserMode, MmCached, Second ? Retained[0].Address : NULL, FALSE,
        NormalPagePriority | (Second ? MdlMappingNoWrite : 0));
    LockedMdl =
        IoAllocateMdl(View + UserMappingByteOffset, 1, FALSE, FALSE, NULL);
    if (!LockedMdl)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(LockedMdl, UserMode,
                        Second ? IoReadAccess : IoWriteAccess);
    Locked = TRUE;
    PUCHAR System = MmGetSystemAddressForMdlSafe(LockedMdl, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    State->Mdl = Mdl;
    State->Pool = Pool;
    State->Address = View;
    State->LockedMdl = LockedMdl;
    State->System = System;
    Report[0] = 1;
    return STATUS_SUCCESS;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (View)
    MmUnmapLockedPages(View, Mdl);
  if (Locked)
    MmUnlockPages(LockedMdl);
  if (LockedMdl)
    IoFreeMdl(LockedMdl);
  return Status;
}

static NTSTATUS ReleaseProcessView(volatile UCHAR *Report, BOOLEAN Second) {
  UserMappingProcessView *State = &Retained[Second ? 1 : 0];
  if (!State->Mdl || (!Second && !Retained[1].Mdl))
    return STATUS_INVALID_DEVICE_STATE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    Report[0] = ((volatile UCHAR *)State->Address)[UserMappingByteOffset];
    if (Second) {
      __try {
        ((volatile UCHAR *)State->Address)[UserMappingByteOffset] = 0;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (GetExceptionCode() == STATUS_ACCESS_VIOLATION)
          Report[1] = 1;
      }
      Report[2] = State->System[0] == UserMappingSecondByte;
    } else {
      Report[1] = Retained[1].System[0];
      Report[2] = State->Address == Retained[1].Address &&
                  MmGetMdlPfnArray(State->LockedMdl)[0] ==
                      MmGetMdlPfnArray(State->Mdl)[0] &&
                  MmGetMdlPfnArray(Retained[1].LockedMdl)[0] ==
                      MmGetMdlPfnArray(Retained[1].Mdl)[0] &&
                  MmGetMdlPfnArray(State->LockedMdl)[0] !=
                      MmGetMdlPfnArray(Retained[1].LockedMdl)[0];
      ((volatile UCHAR *)State->Address)[UserMappingByteOffset] =
          UserMappingWorkerByte;
      Report[3] = State->System[0] == UserMappingWorkerByte &&
                  Retained[1].System[0] == UserMappingSecondByte;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  MmUnmapLockedPages(State->Address, State->Mdl);
  MmUnlockPages(State->LockedMdl);
  IoFreeMdl(State->LockedMdl);
  ReleasePool(State->Mdl, State->Pool);
  RtlZeroMemory(State, sizeof(*State));
  return Status;
}

static NTSTATUS CheckPartialReuse(PMDL *OriginalMdl, PUCHAR Pool,
                                  volatile UCHAR *Report) {
  enum {
    ViewLength = 16,
    FirstOffset = PAGE_SIZE - 8,
    SecondOffset = PAGE_SIZE + 16,
    InheritedOffset = 32
  };
  PMDL Source = NULL, Partial = NULL, Leaf = NULL;
  PUCHAR UserView = NULL;
  BOOLEAN Locked = FALSE;
  NTSTATUS Status = STATUS_SUCCESS;
  __try {
    UserView = MmMapLockedPagesSpecifyCache(*OriginalMdl, UserMode, MmCached,
                                            NULL, FALSE, NormalPagePriority);
    Source = IoAllocateMdl(UserView, 2 * PAGE_SIZE, FALSE, FALSE, NULL);
    Partial = IoAllocateMdl(UserView, 2 * PAGE_SIZE, FALSE, FALSE, NULL);
    Leaf = IoAllocateMdl(UserView, 2 * PAGE_SIZE, FALSE, FALSE, NULL);
    if (!Source || !Partial || !Leaf)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    MmProbeAndLockPages(Source, UserMode, IoWriteAccess);
    Locked = TRUE;
    Partial->Next = Leaf;
    IoBuildPartialMdl(Source, Partial, UserView + FirstOffset, ViewLength);
    if (MmGetMdlVirtualAddress(Partial) == UserView + FirstOffset &&
        MmGetMdlByteCount(Partial) == ViewLength)
      Report[2] |= PartialOriginalRange;
    if (MmGetMdlPfnArray(Partial)[0] == MmGetMdlPfnArray(Source)[0] &&
        MmGetMdlPfnArray(Partial)[1] == MmGetMdlPfnArray(Source)[1])
      Report[2] |= PartialPhysicalPages;
    PUCHAR System = MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    System[0] = UserMappingFirstByte;
    Report[0] = Pool[FirstOffset];
    MmPrepareMdlForReuse(Partial);
    if ((Partial->MdlFlags & MDL_PARTIAL) &&
        !(Partial->MdlFlags &
          (MDL_PARTIAL_HAS_BEEN_MAPPED | MDL_MAPPED_TO_SYSTEM_VA)) &&
        MmGetMdlByteCount(Partial) == ViewLength && Partial->Next == Leaf &&
        MmGetMdlPfnArray(Partial)[1] == MmGetMdlPfnArray(Source)[1])
      Report[2] |= PartialPreparedDescriptor;
    IoBuildPartialMdl(Source, Partial, UserView + SecondOffset, ViewLength);
    System = MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority);
    if (!System)
      ExRaiseStatus(STATUS_INSUFFICIENT_RESOURCES);
    System[0] = UserMappingSecondByte;
    Report[1] = Pool[SecondOffset];
    if (MmGetMdlVirtualAddress(Partial) == UserView + SecondOffset &&
        MmGetMdlPfnArray(Partial)[0] == MmGetMdlPfnArray(Source)[1] &&
        Partial->Next == Leaf)
      Report[2] |= PartialReusedRange;
    MmPrepareMdlForReuse(Partial);

    // The nonpaged source lends an existing system mapping. The genuine WDK
    // helpers must preserve it without calling MmMapLockedPages again.
    IoBuildPartialMdl(*OriginalMdl, Partial, Pool + InheritedOffset,
                      ViewLength);
    const CSHORT InheritedFlags =
        MDL_MAPPED_TO_SYSTEM_VA | MDL_PARENT_MAPPED_SYSTEM_VA;
    if ((Partial->MdlFlags & InheritedFlags) == InheritedFlags &&
        !(Partial->MdlFlags & MDL_PARTIAL_HAS_BEEN_MAPPED) &&
        MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority) ==
            Pool + InheritedOffset)
      Report[3] |= PartialInheritedMapping;
    MmPrepareMdlForReuse(Partial);
    if (MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority) ==
        Pool + InheritedOffset)
      Report[3] |= PartialInheritedPrepare;
    IoBuildPartialMdl(Partial, Leaf, Pool + InheritedOffset + 1,
                      ViewLength - 1);
    MmUnmapLockedPages(UserView, *OriginalMdl);
    UserView = NULL;
    MmUnlockPages(Source);
    Locked = FALSE;
    IoFreeMdl(Source);
    Source = NULL;
    IoFreeMdl(*OriginalMdl);
    *OriginalMdl = NULL;
    if (MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority) ==
        Pool + InheritedOffset)
      Report[3] |= PartialSourceReleased;
    IoFreeMdl(Partial);
    Partial = NULL;
    if (MmGetSystemAddressForMdlSafe(Leaf, NormalPagePriority) ==
        Pool + InheritedOffset + 1)
      Report[3] |= PartialDescendantSurvives;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (Leaf)
    IoFreeMdl(Leaf);
  if (Partial)
    IoFreeMdl(Partial);
  if (Locked)
    MmUnlockPages(Source);
  if (Source)
    IoFreeMdl(Source);
  if (UserView)
    MmUnmapLockedPages(UserView, *OriginalMdl);
  return Status;
}

static NTSTATUS CheckShortage(PMDL Mdl, volatile UCHAR *Report) {
  enum { MaximumViews = 128 };
  PVOID Views[MaximumViews];
  ULONG Count = 0;
  NTSTATUS Status = STATUS_UNSUCCESSFUL;
  __try {
    while (Count < MaximumViews) {
      Views[Count] = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL,
                                                  FALSE, NormalPagePriority);
      ++Count;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (GetExceptionCode() == STATUS_INSUFFICIENT_RESOURCES && Count) {
      Report[0] = 1;
      Status = STATUS_SUCCESS;
    }
  }
  while (Count)
    MmUnmapLockedPages(Views[--Count], Mdl);
  return Status;
}

static NTSTATUS CheckIndependentPages(volatile UCHAR *Report, ULONG Action) {
  PHYSICAL_ADDRESS Low = {0}, High, Skip = {0};
  High.QuadPart = MAXLONGLONG;
  const BOOLEAN Chunks = Action == UserMappingContiguousChunks;
  const SIZE_T Size = (Chunks ? 4 : 2) * PAGE_SIZE;
  const ULONG Flags = MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_NO_WAIT |
                      (Chunks ? MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS : 0);
  if (Chunks)
    Skip.QuadPart = 2 * PAGE_SIZE;
  PMDL Mdl = Action == UserMappingLegacyPages
                 ? MmAllocatePagesForMdl(Low, High, Skip, Size)
                 : MmAllocatePagesForMdlEx(Low, High, Skip, Size,
                                           MmWriteCombined, Flags);
  if (!Mdl)
    return STATUS_INSUFFICIENT_RESOURCES;
  if (MmGetMdlByteCount(Mdl) != Size) {
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  NTSTATUS Status = STATUS_SUCCESS;
  if (Chunks) {
    const PPFN_NUMBER Pages = MmGetMdlPfnArray(Mdl);
    for (ULONG Index = 0; Index != 4; Index += 2)
      if (Pages[Index] % 2 || Pages[Index + 1] != Pages[Index] + 1)
        Status = STATUS_INVALID_ADDRESS;
  }
  PUCHAR User = NULL;
  __try {
    volatile UCHAR *System =
        MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmNonCached, NULL, FALSE,
                                     NormalPagePriority | MdlMappingNoExecute);
    if (!System)
      Status = STATUS_INSUFFICIENT_RESOURCES;
    else {
      if (MmProtectMdlSystemAddress(Mdl, PAGE_READONLY) != STATUS_SUCCESS)
        Status = STATUS_UNSUCCESSFUL;
      Report[0] = System[UserMappingByteOffset] == 0 &&
                  System[PAGE_SIZE + UserMappingByteOffset] == 0;
      if (MmProtectMdlSystemAddress(Mdl, PAGE_READWRITE) != STATUS_SUCCESS)
        Status = STATUS_UNSUCCESSFUL;
      System[UserMappingByteOffset] = UserMappingFirstByte;
      User = MmMapLockedPagesSpecifyCache(Mdl, UserMode, MmCached, NULL, FALSE,
                                          NormalPagePriority);
      User[PAGE_SIZE + UserMappingByteOffset] = UserMappingSecondByte;
      Report[1] = User[UserMappingByteOffset];
      Report[2] = System[PAGE_SIZE + UserMappingByteOffset];
      Report[3] =
          MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority) == System;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Status = GetExceptionCode();
  }
  if (User)
    MmUnmapLockedPages(User, Mdl);
  // Modern WDM releases an existing system mapping with these physical pages.
  // The separately allocated descriptor remains ours until ExFreePool.
  MmFreePagesFromMdl(Mdl);
  ExFreePool(Mdl);
  return Status;
}

static NTSTATUS CheckKernelPoolLock(volatile UCHAR *Report) {
  NTSTATUS Status = STATUS_SUCCESS;
  for (ULONG Kind = 0; Kind != 2; ++Kind) {
    const POOL_FLAGS Flags = Kind ? POOL_FLAG_NON_PAGED : POOL_FLAG_PAGED;
    PUCHAR Pool = ExAllocatePool2(Flags, PAGE_SIZE, UserMappingPoolTag);
    if (!Pool)
      return STATUS_INSUFFICIENT_RESOURCES;
    PMDL Mdl =
        IoAllocateMdl(Pool + UserMappingByteOffset, 1, FALSE, FALSE, NULL);
    if (!Mdl) {
      ExFreePoolWithTag(Pool, UserMappingPoolTag);
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    KIRQL Previous = PASSIVE_LEVEL;
    if (Kind)
      KeRaiseIrql(DISPATCH_LEVEL, &Previous);
    MmProbeAndLockPages(Mdl, KernelMode, IoWriteAccess);
    volatile UCHAR *Alias =
        MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    if (!Kind)
      KeRaiseIrql(DISPATCH_LEVEL, &Previous);
    UCHAR Observed = 0;
    if (Alias) {
      *Alias = UserMappingFirstByte + (UCHAR)Kind;
      Observed = Pool[UserMappingByteOffset];
    } else
      Status = STATUS_INSUFFICIENT_RESOURCES;
    MmUnlockPages(Mdl);
    KeLowerIrql(Previous);
    Report[Kind] = Observed;
    Report[Kind + 2] = (Mdl->MdlFlags & MDL_PAGES_LOCKED) == 0;
    IoFreeMdl(Mdl);
    ExFreePoolWithTag(Pool, UserMappingPoolTag);
  }
  return Status;
}

static NTSTATUS CompleteIndependentPartial(PIRP Irp, volatile UCHAR *Report) {
  PHYSICAL_ADDRESS Low = {0}, High, Skip = {0};
  High.QuadPart = MAXLONGLONG;
  PMDL Source = MmAllocatePagesForMdlEx(Low, High, Skip, 2 * PAGE_SIZE,
                                        MmCached, MM_ALLOCATE_FULLY_REQUIRED);
  if (!Source)
    return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
  PVOID Range = (PVOID)((ULONG_PTR)MmGetMdlVirtualAddress(Source) + PAGE_SIZE);
  PMDL Partial = IoAllocateMdl(Range, PAGE_SIZE, FALSE, FALSE, Irp);
  NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
  if (Partial) {
    IoBuildPartialMdl(Source, Partial, Range, PAGE_SIZE);
    volatile UCHAR *Bytes =
        MmGetSystemAddressForMdlSafe(Partial, NormalPagePriority);
    if (Bytes) {
      Report[0] = MmGetMdlPfnArray(Partial)[0] == MmGetMdlPfnArray(Source)[1];
      Bytes[UserMappingByteOffset] = UserMappingFirstByte;
      Report[1] = Bytes[UserMappingByteOffset];
      Report[2] = (Partial->MdlFlags & MDL_PARTIAL) != 0;
      Report[3] = MmGetMdlByteCount(Source) == 2 * PAGE_SIZE;
      Status = STATUS_SUCCESS;
    }
  }
  // The I/O manager owns only the partial descriptor. Its completion releases
  // the partial mapping, after which our independent physical pages can retire.
  Complete(Irp, Status);
  MmFreePagesFromMdl(Source);
  ExFreePool(Source);
  return Status;
}

static NTSTATUS CheckCreatedDescriptors(volatile UCHAR *Report,
                                        PMDL StackStorage) {
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, L"MmCreateMdl");
  PMDL(NTAPI * Create)(PMDL, PVOID, SIZE_T) =
      (PMDL(NTAPI *)(PMDL, PVOID, SIZE_T))MmGetSystemRoutineAddress(&Name);
  if (!Create)
    return STATUS_PROCEDURE_NOT_FOUND;
  PUCHAR Pool =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, PAGE_SIZE, UserMappingPoolTag);
  PUCHAR Storage =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, 128, UserMappingPoolTag);
  if (!Pool || !Storage) {
    if (Pool)
      ExFreePoolWithTag(Pool, UserMappingPoolTag);
    if (Storage)
      ExFreePoolWithTag(Storage, UserMappingPoolTag);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  Pool[UserMappingByteOffset] = UserMappingFirstByte;
  PMDL Owned = MmCreateMdl(NULL, Pool + UserMappingByteOffset, 4);
  if (!Owned)
    return STATUS_INSUFFICIENT_RESOURCES;
  if (MmGetMdlByteCount(Owned) == 4 &&
      MmGetMdlVirtualAddress(Owned) == Pool + UserMappingByteOffset)
    Report[0] |= 1;
  MmBuildMdlForNonPagedPool(Owned);
  if (MmGetSystemAddressForMdlSafe(Owned, NormalPagePriority) ==
      Pool + UserMappingByteOffset)
    Report[0] |= 2;
  IoFreeMdl(Owned);
  PMDL Empty = Create(NULL, NULL, 0);
  if (!Empty)
    return STATUS_INSUFFICIENT_RESOURCES;
  if (MmGetMdlByteCount(Empty) == 0 && !MmGetMdlVirtualAddress(Empty))
    Report[0] |= 4;
  ExFreePool(Empty);

  RtlFillMemory(Storage, 128, 0xa5);
  PMDL Borrowed = Create((PMDL)(Storage + 16), Pool + UserMappingByteOffset, 4);
  if (Borrowed == (PMDL)(Storage + 16) && MmGetMdlByteCount(Borrowed) == 4)
    Report[1] |= 1;
  MmBuildMdlForNonPagedPool(Borrowed);
  if (MmGetMdlPfnArray(Borrowed)[0] ==
      (PFN_NUMBER)(MmGetPhysicalAddress(Pool).QuadPart >> PAGE_SHIFT))
    Report[1] |= 2;
  ExFreePoolWithTag(Storage, UserMappingPoolTag);
  if (Pool[UserMappingByteOffset] == UserMappingFirstByte)
    Report[1] |= 4;

  PMDL Local = MmCreateMdl(StackStorage, Pool + UserMappingByteOffset, 4);
  if (Local == StackStorage && MmGetMdlByteCount(Local) == 4)
    Report[2] |= 1;
  MmBuildMdlForNonPagedPool(Local);
  PUCHAR System = MmGetSystemAddressForMdlSafe(Local, NormalPagePriority);
  if (System == Pool + UserMappingByteOffset &&
      System[0] == UserMappingFirstByte)
    Report[2] |= 2;
  if (!Create((PMDL)(ULONG_PTR)-1, Pool, (SIZE_T)-1))
    Report[3] |= 1;
  ExFreePoolWithTag(Pool, UserMappingPoolTag);
  // StackStorage belongs to Dispatch and survives this helper. The execution
  // stack retires it after dispatch returns; it must never reach IoFreeMdl.
  return STATUS_SUCCESS;
}

static NTSTATUS CheckAdvancedDescriptor(volatile UCHAR *Report) {
  UNICODE_STRING Name;
  RtlInitUnicodeString(&Name, L"MmAdvanceMdl");
  NTSTATUS(NTAPI * Advance)(PMDL, ULONG) =
      (NTSTATUS(NTAPI *)(PMDL, ULONG))MmGetSystemRoutineAddress(&Name);
  if (!Advance)
    return STATUS_PROCEDURE_NOT_FOUND;
  PUCHAR Pool =
      ExAllocatePool2(POOL_FLAG_NON_PAGED, 4 * PAGE_SIZE, UserMappingPoolTag);
  if (!Pool)
    return STATUS_INSUFFICIENT_RESOURCES;
  PMDL Mdl = MmCreateMdl(NULL, Pool + 17, 3 * PAGE_SIZE);
  NTSTATUS Status = STATUS_INSUFFICIENT_RESOURCES;
  BOOLEAN Locked = FALSE;
  if (!Mdl)
    goto Cleanup;
  MmProbeAndLockPages(Mdl, KernelMode, IoWriteAccess);
  Locked = TRUE;
  PUCHAR Initial = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
  if (!Initial)
    goto Cleanup;
  PFN_NUMBER Original[4];
  for (ULONG I = 0; I < 4; ++I)
    Original[I] = MmGetMdlPfnArray(Mdl)[I];
  if (NT_SUCCESS(MmAdvanceMdl(Mdl, 17)))
    Report[0] |= 1;
  if (NT_SUCCESS(Advance(Mdl, PAGE_SIZE)))
    Report[0] |= 2;
  if (NT_SUCCESS(MmAdvanceMdl(Mdl, PAGE_SIZE - 1)))
    Report[0] |= 4;
  PUCHAR Current = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
  const ULONG Total = 2 * PAGE_SIZE + 16;
  if (Current == Initial + Total &&
      MmGetMdlVirtualAddress(Mdl) == Pool + 17 + Total &&
      MmGetMdlByteCount(Mdl) == 3 * PAGE_SIZE - Total)
    Report[1] |= 1;
  Current[0] = UserMappingSecondByte;
  if (Pool[17 + Total] == UserMappingSecondByte)
    Report[1] |= 2;
  if (MmGetMdlPfnArray(Mdl)[0] == Original[2] &&
      MmGetMdlPfnArray(Mdl)[1] == Original[3] &&
      MmGetMdlPfnArray(Mdl)[2] == Original[0] &&
      MmGetMdlPfnArray(Mdl)[3] == (Original[1] | ((PFN_NUMBER)1 << 63)))
    Report[1] |= 4;
  if (MmAdvanceMdl(Mdl, MmGetMdlByteCount(Mdl)) == STATUS_INVALID_PARAMETER_2)
    Report[2] |= 1;
  if (NT_SUCCESS(MmAdvanceMdl(Mdl, 0)))
    Report[2] |= 2;
  MmUnmapLockedPages(Current, Mdl);
  BOOLEAN Restored = Mdl->StartVa == Pool && Mdl->ByteOffset == 33 &&
                     Mdl->ByteCount == 3 * PAGE_SIZE - 16 &&
                     Mdl->MappedSystemVa == Initial + 16 &&
                     Mdl->MdlFlags == MDL_PAGES_LOCKED;
  for (ULONG I = 0; I < 4; ++I)
    Restored = Restored && MmGetMdlPfnArray(Mdl)[I] == Original[I];
  if (Restored)
    Report[2] |= 4;
  MmUnlockPages(Mdl);
  Locked = FALSE;
  IoFreeMdl(Mdl);
  Mdl = IoAllocateMdl(Pool + 17, 3 * PAGE_SIZE, FALSE, FALSE, NULL);
  if (!Mdl)
    goto Cleanup;
  MmProbeAndLockPages(Mdl, KernelMode, IoWriteAccess);
  Locked = TRUE;
  if (!MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority))
    goto Cleanup;
  if (!NT_SUCCESS(MmAdvanceMdl(Mdl, PAGE_SIZE)))
    goto Cleanup;
  MmUnlockPages(Mdl);
  Locked = FALSE;
  if (!Mdl->MdlFlags && MmGetMdlVirtualAddress(Mdl) == Pool + 17 &&
      Mdl->ByteCount == 3 * PAGE_SIZE &&
      Pool[17 + Total] == UserMappingSecondByte)
    Report[3] |= 1;
  Status = STATUS_SUCCESS;
Cleanup:
  if (Locked)
    MmUnlockPages(Mdl);
  if (Mdl)
    IoFreeMdl(Mdl);
  ExFreePoolWithTag(Pool, UserMappingPoolTag);
  return Status;
}

static NTSTATUS Dispatch(PDEVICE_OBJECT Object, PIRP Irp) {
  PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
  UNREFERENCED_PARAMETER(Object);
  if (Stack->MajorFunction != IRP_MJ_DEVICE_CONTROL)
    return Complete(Irp, STATUS_SUCCESS);
  const ULONG Action = Stack->Parameters.DeviceIoControl.IoControlCode;
  if (Stack->Parameters.DeviceIoControl.InputBufferLength !=
      sizeof(DriverUserMappingRequest))
    return Complete(Irp, STATUS_INVALID_PARAMETER);
  DriverUserMappingRequest *Request =
      Stack->Parameters.DeviceIoControl.Type3InputBuffer;
  volatile UCHAR *Report;
  __try {
    ProbeForRead(Request, sizeof(*Request), TYPE_ALIGNMENT(PVOID));
    Report = Request->Report;
    ProbeForWrite((PVOID)Report, UserMappingReportSize, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return Complete(Irp, GetExceptionCode());
  }
  if (Action == UserMappingAdvanceMdl)
    return Complete(Irp, CheckAdvancedDescriptor(Report));
  if (Action == UserMappingCreateMdl) {
    struct {
      MDL Descriptor;
      PFN_NUMBER Pages[2];
    } Storage;
    return Complete(Irp, CheckCreatedDescriptors(Report, &Storage.Descriptor));
  }
  if (Action == UserMappingIndependentPages ||
      Action == UserMappingLegacyPages || Action == UserMappingContiguousChunks)
    return Complete(Irp, CheckIndependentPages(Report, Action));
  if (Action == UserMappingKernelPoolLock)
    return Complete(Irp, CheckKernelPoolLock(Report));
  if (Action == UserMappingIndependentPartial)
    return CompleteIndependentPartial(Irp, Report);
  if (Action == UserMappingReleaseFirstProcessView ||
      Action == UserMappingReleaseSecondProcessView)
    return Complete(
        Irp, ReleaseProcessView(Report,
                                Action == UserMappingReleaseSecondProcessView));
  const ULONG Size = Action == UserMappingShortage       ? 32 * PAGE_SIZE
                     : Action == UserMappingPartialReuse ? 2 * PAGE_SIZE
                                                         : PAGE_SIZE;
  PUCHAR Pool = ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, UserMappingPoolTag);
  if (!Pool)
    return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
  PMDL Mdl = IoAllocateMdl(Pool, Size, FALSE, FALSE, NULL);
  if (!Mdl) {
    ExFreePoolWithTag(Pool, UserMappingPoolTag);
    return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
  }
  MmBuildMdlForNonPagedPool(Mdl);
  NTSTATUS Status;
  if (Action == UserMappingPoolAliases)
    Status = CheckAliases(Mdl, Pool, Report);
  else if (Action == UserMappingPhysicalAddress ||
           Action == UserMappingPrimaryAddress)
    Status = CheckPhysicalAddresses(Mdl, Pool, Report,
                                    Action == UserMappingPrimaryAddress);
  else if (Action == UserMappingShortage)
    Status = CheckShortage(Mdl, Report);
  else if (Action == UserMappingPartialReuse)
    Status = CheckPartialReuse(&Mdl, Pool, Report);
  else if (Action == UserMappingAddressReuse)
    Status = CheckAddressReuse(Mdl, Pool, Report);
  else if (Action == UserMappingRetainProcessView ||
           Action == UserMappingConcurrentProcessView) {
    Status = RetainProcessView(Mdl, Pool, Report,
                               Action == UserMappingConcurrentProcessView);
    if (NT_SUCCESS(Status))
      return Complete(Irp, Status);
  } else if (Action == UserMappingAttachedWorker ||
             Action == UserMappingProcessLock ||
             Action == UserMappingProcessExit ||
             Action == UserMappingWrongProcessUnmap) {
    PMDL ReportMdl =
        IoAllocateMdl((PVOID)Report, UserMappingReportSize, FALSE, FALSE, NULL);
    PIO_WORKITEM Work = IoAllocateWorkItem(Device);
    if (!ReportMdl || !Work) {
      if (ReportMdl)
        IoFreeMdl(ReportMdl);
      if (Work)
        IoFreeWorkItem(Work);
      ReleasePool(Mdl, Pool);
      return Complete(Irp, STATUS_INSUFFICIENT_RESOURCES);
    }
    MmProbeAndLockPages(ReportMdl, UserMode, IoWriteAccess);
    Pending.Irp = Irp;
    Pending.Work = Work;
    Pending.Process = IoGetRequestorProcess(Irp);
    Pending.PoolMdl = Mdl;
    Pending.ReportMdl = ReportMdl;
    Pending.Pool = Pool;
    Pending.Report =
        MmGetSystemAddressForMdlSafe(ReportMdl, NormalPagePriority);
    Pending.UserReport = (PUCHAR)Report;
    Pending.Action = Action;
    Pool[UserMappingByteOffset] = UserMappingFirstByte;
    if (Action != UserMappingAttachedWorker)
      Pending.UserView = MmMapLockedPagesSpecifyCache(
          Mdl, UserMode, MmCached, NULL, FALSE,
          NormalPagePriority |
              (Action == UserMappingProcessLock ? MdlMappingNoWrite : 0));
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(Work, MappingWorker, DelayedWorkQueue, &Pending);
    return STATUS_PENDING;
  } else
    Status = STATUS_INVALID_DEVICE_REQUEST;
  ReleasePool(Mdl, Pool);
  return Complete(Irp, Status);
}

static VOID Unload(PDRIVER_OBJECT Driver) {
  UNREFERENCED_PARAMETER(Driver);
  IoDeleteDevice(Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Path) {
  UNICODE_STRING Name;
  UNREFERENCED_PARAMETER(Path);
  RtlInitUnicodeString(&Name, L"\\Device\\NeverDUserMapping");
  NTSTATUS Status =
      IoCreateDevice(Driver, 0, &Name, FILE_DEVICE_UNKNOWN, 0, FALSE, &Device);
  if (!NT_SUCCESS(Status))
    return Status;
  Device->Flags &= ~DO_DEVICE_INITIALIZING;
  Driver->DriverUnload = Unload;
  Driver->MajorFunction[IRP_MJ_CREATE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLEANUP] = Dispatch;
  Driver->MajorFunction[IRP_MJ_CLOSE] = Dispatch;
  Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = Dispatch;
  return STATUS_SUCCESS;
}

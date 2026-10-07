//===- driver_wdm_multiple_wait.c - Genuine WDK wait-set fixture ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include <ntddk.h>

#define NEVERD_MULTI_WAIT_VALUE(Name, Value) static const ULONG64 Name = Value;
#define NEVERD_MULTI_WAIT_CASE(Name, Value) enum { Name = Value };
#define NEVERD_MULTI_WAIT_TEXT(Name, Value) static const char Name[] = Value;
#define NEVERD_MULTI_WAIT_ABI(Condition, Message)                              \
  _Static_assert(Condition, Message);
#include "DriverMultipleWaitCases.def"
#undef NEVERD_MULTI_WAIT_ABI
#undef NEVERD_MULTI_WAIT_TEXT
#undef NEVERD_MULTI_WAIT_CASE
#undef NEVERD_MULTI_WAIT_VALUE

// C array extents need integer constant expressions, independently checked
// against the WDK limits above.
static KEVENT Events[MAXIMUM_WAIT_OBJECTS];
static KMUTEX Mutex;
static KSEMAPHORE Semaphore;
static KTIMER Timer;
static ULONG Invalid;
static UCHAR Mode;
static volatile ULONG WorkSeen, StopSeen, Completed;

static void Check(BOOLEAN Condition) {
  if (!Condition)
    ++Invalid;
}

static NTSTATUS Multiple(ULONG Count, PVOID *Objects, WAIT_TYPE Type,
                         PLARGE_INTEGER Timeout, PKWAIT_BLOCK Blocks) {
  return KeWaitForMultipleObjects(Count, Objects, Type, Executive, KernelMode,
                                  FALSE, Timeout, Blocks);
}

static void WaitEvent(ULONG Index) {
  Check(KeWaitForSingleObject(&Events[Index], Executive, KernelMode, FALSE,
                              NULL) == STATUS_SUCCESS);
}

static void Worker(PVOID Context) {
  UNREFERENCED_PARAMETER(Context);
  KeLeaveCriticalRegion();
  if (Mode == AtomicAll) {
    LARGE_INTEGER Zero = {0};
    Check(KeReadStateEvent(&Events[0]) == 1 &&
          KeReadStateSemaphore(&Semaphore) == 1 &&
          KeReadStateMutex(&Mutex) == 1);
    Check(KeWaitForSingleObject(&Mutex, Executive, KernelMode, FALSE, &Zero) ==
          STATUS_SUCCESS);
    Check(KeReleaseMutex(&Mutex, FALSE) == 0);
    KeSetEvent(&Events[1], IO_NO_INCREMENT, FALSE);
  } else if (Mode == WorkerOrStop) {
    PVOID Objects[] = {&Events[0], &Events[1]};
    KeSetEvent(&Events[2], IO_NO_INCREMENT, FALSE);
    Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, NULL, NULL) ==
          STATUS_WAIT_0);
    ++WorkSeen;
    KeSetEvent(&Events[3], IO_NO_INCREMENT, FALSE);
    Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, NULL, NULL) ==
          STATUS_WAIT_1);
    ++StopSeen;
  } else {
    LARGE_INTEGER Interval;
    Interval.QuadPart = -(LONGLONG)CompletionTicks;
    Check(KeDelayExecutionThread(KernelMode, FALSE, &Interval) ==
          STATUS_SUCCESS);
    if (Mode == TimeoutAll)
      KeSetEvent(&Events[1], IO_NO_INCREMENT, FALSE);
  }
  Completed = 1;
  PsTerminateSystemThread(STATUS_SUCCESS);
}

static PVOID StartWorker(PHANDLE Handle) {
  OBJECT_ATTRIBUTES Attributes;
  InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
  Check(PsCreateSystemThread(Handle, THREAD_ALL_ACCESS, &Attributes, NULL, NULL,
                             Worker, NULL) == STATUS_SUCCESS);
  PVOID Object = NULL;
  Check(ObReferenceObjectByHandle(*Handle, 0, NULL, KernelMode, &Object,
                                  NULL) == STATUS_SUCCESS);
  return Object;
}

static void JoinWorker(HANDLE Handle, PVOID Object) {
  PVOID Objects[] = {Object};
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAll, NULL, NULL) ==
        STATUS_SUCCESS);
  Check(Completed == 1);
  Check(ZwClose(Handle) == STATUS_SUCCESS);
  ObDereferenceObject(Object);
}

static void Immediate(void) {
  LARGE_INTEGER Zero = {0};
  KeSetEvent(&Events[0], IO_NO_INCREMENT, FALSE);
  KeSetEvent(&Events[1], IO_NO_INCREMENT, FALSE);
  KeInitializeEvent(&Events[2], NotificationEvent, TRUE);
  PVOID Objects[] = {&Events[0], &Events[1], &Events[2]};
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, &Zero, NULL) ==
        STATUS_WAIT_0);
  Check(KeReadStateEvent(&Events[0]) == 0 && KeReadStateEvent(&Events[1]) == 1);
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, &Zero, NULL) ==
        STATUS_WAIT_1);
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, &Zero, NULL) ==
        STATUS_WAIT_2);
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, &Zero, NULL) ==
        STATUS_WAIT_2);
  Check(KeReadStateEvent(&Events[2]) == 1);
}

static void All(void) {
  HANDLE Handle;
  KeSetEvent(&Events[0], IO_NO_INCREMENT, FALSE);
  KeInitializeSemaphore(&Semaphore, 1, 1);
  KeInitializeMutex(&Mutex, 0);
  PVOID Object = StartWorker(&Handle);
  PVOID Objects[] = {&Events[0], &Semaphore, &Mutex, &Events[1]};
  KWAIT_BLOCK Blocks[RTL_NUMBER_OF(Objects)];
  KIRQL Old;
  KeRaiseIrql(APC_LEVEL, &Old);
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAll, NULL, Blocks) ==
        STATUS_SUCCESS);
  Check(KeGetCurrentIrql() == APC_LEVEL && KeAreApcsDisabled());
  Check(KeReadStateEvent(&Events[0]) == 0 &&
        KeReadStateEvent(&Events[1]) == 0 &&
        KeReadStateSemaphore(&Semaphore) == 0 && KeReadStateMutex(&Mutex) == 0);
  Check(KeReleaseMutex(&Mutex, FALSE) == 0);
  KeLowerIrql(Old);
  Check(!KeAreApcsDisabled());
  JoinWorker(Handle, Object);
}

static void WorkOrStop(void) {
  HANDLE Handle;
  PVOID Object = StartWorker(&Handle);
  WaitEvent(2);
  KeSetEvent(&Events[0], IO_NO_INCREMENT, FALSE);
  WaitEvent(3);
  Check(WorkSeen == 1 && StopSeen == 0);
  KeSetEvent(&Events[1], IO_NO_INCREMENT, FALSE);
  JoinWorker(Handle, Object);
  Check(WorkSeen == 1 && StopSeen == 1);
}

static void Timed(void) {
  HANDLE Handle;
  PVOID Object = StartWorker(&Handle);
  LARGE_INTEGER Due;
  Due.QuadPart = -(LONGLONG)TimerTicks;
  KeInitializeTimerEx(&Timer, SynchronizationTimer);
  Check(!KeSetTimerEx(&Timer, Due, 0, NULL));
  PVOID Objects[] = {Object, &Timer};
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, NULL, NULL) ==
        STATUS_WAIT_1);
  Check(!Completed && KeReadStateTimer(&Timer) == 0);
  JoinWorker(Handle, Object);
  Check(!KeCancelTimer(&Timer));
}

static void Timeout(void) {
  HANDLE Handle;
  KeSetEvent(&Events[0], IO_NO_INCREMENT, FALSE);
  PVOID Object = StartWorker(&Handle);
  PVOID Objects[] = {&Events[0], &Events[1]};
  LARGE_INTEGER Due;
  Due.QuadPart = -(LONGLONG)TimeoutTicks;
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAll, &Due, NULL) ==
        STATUS_TIMEOUT);
  Check(!Completed && KeReadStateEvent(&Events[0]) == 1);
  JoinWorker(Handle, Object);
  Due.QuadPart = 0;
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAll, &Due, NULL) ==
        STATUS_SUCCESS);
}

static void Full(void) {
  struct {
    ULONG64 Before;
    KWAIT_BLOCK Blocks[MAXIMUM_WAIT_OBJECTS];
    ULONG64 After;
  } Storage;
  PVOID Objects[MAXIMUM_WAIT_OBJECTS];
  for (ULONG I = 0; I < MAXIMUM_WAIT_OBJECTS; ++I)
    Objects[I] = &Events[I];
  Storage.Before = Storage.After = GuardByte;
  KeSetEvent(&Events[MAXIMUM_WAIT_OBJECTS - 1], IO_NO_INCREMENT, FALSE);
  LARGE_INTEGER Zero = {0};
  Check(Multiple(MAXIMUM_WAIT_OBJECTS, Objects, WaitAny, &Zero,
                 Storage.Blocks) == STATUS_WAIT_0 + MAXIMUM_WAIT_OBJECTS - 1);
  for (ULONG I = 0; I < MAXIMUM_WAIT_OBJECTS; ++I)
    KeSetEvent(&Events[I], IO_NO_INCREMENT, FALSE);
  Check(Multiple(MAXIMUM_WAIT_OBJECTS, Objects, WaitAll, &Zero,
                 Storage.Blocks) == STATUS_SUCCESS);
  for (ULONG I = 0; I < MAXIMUM_WAIT_OBJECTS; ++I)
    Check(KeReadStateEvent(&Events[I]) == 0);
  Check(Storage.Before == GuardByte && Storage.After == GuardByte);
}

static void Poll(void) {
  KeSetEvent(&Events[0], IO_NO_INCREMENT, FALSE);
  PVOID Objects[] = {&Events[0], &Events[1]};
  LARGE_INTEGER Zero = {0};
  KIRQL Old;
  KeRaiseIrql(DISPATCH_LEVEL, &Old);
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAll, &Zero, NULL) ==
        STATUS_TIMEOUT);
  Check(KeReadStateEvent(&Events[0]) == 1);
  Check(Multiple(RTL_NUMBER_OF(Objects), Objects, WaitAny, &Zero, NULL) ==
        STATUS_WAIT_0);
  Check(KeGetCurrentIrql() == DISPATCH_LEVEL);
  KeLowerIrql(Old);
}

static void Unload(PDRIVER_OBJECT Driver) {
  UNREFERENCED_PARAMETER(Driver);
  DbgPrint(Complete);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING Registry) {
  if (!Registry || Registry->Length < sizeof(WCHAR))
    return STATUS_INVALID_PARAMETER;
  Mode = ImmediateAny;
  if (Registry->Length >= ModeCharacters * sizeof(WCHAR) &&
      Registry->Buffer[Registry->Length / sizeof(WCHAR) - ModeCharacters] ==
          ModeMarker)
    Mode = (UCHAR)Registry->Buffer[Registry->Length / sizeof(WCHAR) - 1];
  for (ULONG I = 0; I < MAXIMUM_WAIT_OBJECTS; ++I)
    KeInitializeEvent(&Events[I], SynchronizationEvent, FALSE);
  switch (Mode) {
  case ImmediateAny:
    Immediate();
    break;
  case AtomicAll:
    All();
    break;
  case WorkerOrStop:
    WorkOrStop();
    break;
  case TimerOrThread:
    Timed();
    break;
  case TimeoutAll:
    Timeout();
    break;
  case FullWaitSet:
    Full();
    break;
  case DispatchPoll:
    Poll();
    break;
  default:
    return STATUS_INVALID_PARAMETER;
  }
  if (Invalid)
    return STATUS_UNSUCCESSFUL;
  Driver->DriverUnload = Unload;
  return STATUS_SUCCESS;
}

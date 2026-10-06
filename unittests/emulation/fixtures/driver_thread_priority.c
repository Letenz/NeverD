//===- driver_thread_priority.c - Priority driver fixture -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#define DriverEntry BufferedDriverEntry
#include "driver_io.c"
#undef DriverEntry

#define NEVERD_DRIVER_PRIORITY_CASE(Name, Value) enum { Name = Value };
#define NEVERD_DRIVER_PRIORITY_VALUE(Name, Value) enum { Name = Value };
#include "DriverThreadPriorityCases.def"
#undef NEVERD_DRIVER_PRIORITY_VALUE
#undef NEVERD_DRIVER_PRIORITY_CASE
#define NEVERD_THREAD_PRIORITY_VALUE(Name, Value)                              \
  enum { Priority##Name = Value };
#include "../../../lib/emulation/os/windows/kernel/KernelThreadPriorities.def"
#undef NEVERD_THREAD_PRIORITY_VALUE
#define NEVERD_WDM_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/os/windows/kernel/KernelValues.def"
#include "../../../lib/emulation/os/windows/kernel/WindowsKernelLayout.def"
#undef NEVERD_WDM_VALUE
#define NEVERD_KERNEL_DISPATCHER_VALUE(Name, Value) enum { Name = Value };
#include "../../../lib/emulation/os/windows/kernel/KernelDispatcherValues.def"
#undef NEVERD_KERNEL_DISPATCHER_VALUE

typedef struct {
  U32 Length;
  void *RootDirectory;
  UNICODE_STRING *ObjectName;
  U32 Attributes;
  void *SecurityDescriptor;
  void *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
typedef struct {
  U64 Data[TimerSize / sizeof(U64)];
} TIMER;
typedef struct {
  U64 Data[EventSize / sizeof(U64)];
} EVENT;
typedef struct {
  U64 Data[DpcSize / sizeof(U64)];
} DPC;

__declspec(dllimport) NTSTATUS PsCreateSystemThread(void **, U32, void *,
                                                    void *, void *,
                                                    void (*)(void *), void *);
__declspec(dllimport) void PsTerminateSystemThread(NTSTATUS);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(void *, U32, void *,
                                                         U8, void **, void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) int KeQueryPriorityThread(void *);
__declspec(dllimport) int KeSetPriorityThread(void *, int);
__declspec(dllimport) void KeLeaveCriticalRegion(void);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) void KeInitializeEvent(EVENT *, U32, U8);
__declspec(dllimport) long KeSetEvent(EVENT *, long, U8);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, U32, U8,
                                                     long long *);
__declspec(dllimport) void KeInitializeTimer(TIMER *);
__declspec(dllimport) U8 KeSetTimerEx(TIMER *, long long, int, DPC *);
__declspec(dllimport) U8 KeCancelTimer(TIMER *);
__declspec(dllimport) void
KeInitializeDpc(DPC *, void (*)(DPC *, void *, void *, void *), void *);
U64 __readgsqword(unsigned long);
#pragma intrinsic(__readgsqword)

static volatile U32 Started[MaxThreads], Finished[MaxThreads];
static volatile U32 AfterSignal, AfterLower, Ticks, Counter, Observed,
    PeerObserved;
static volatile U32 Invalid;
static U32 Mode;
static void *Objects[MaxThreads];
static EVENT Event;
static TIMER Timer;
static DPC Dpc;

static void *CurrentThread(void) {
  return (void *)__readgsqword(GSCurrentThreadOffset);
}
static void Check(U32 Condition) {
  if (!Condition)
    Invalid = 1;
}
static void Busy(U32 Iterations) {
  for (volatile U32 I = 0; I < Iterations; ++I) {
  }
}
static void Tick(DPC *Call, void *Context, void *First, void *Second) {
  (void)Call;
  (void)Context;
  (void)First;
  (void)Second;
  Check(KeGetCurrentIrql() == DispatchLevel && !Started[0]);
  ++Ticks;
}
// Keep both compared worker entry paths independent of Mode: even a different
// branch before the loop would consume a different part of the first quantum.
static void RemainderCounter(void *Context) {
  (void)Context;
  KeLeaveCriticalRegion();
  Started[1] = 1;
  while (!PeerObserved)
    ++Counter;
  Finished[1] = 1;
  PsTerminateSystemThread(StatusSuccess);
}
static void RemainderPeer(void *Context) {
  (void)Context;
  KeLeaveCriticalRegion();
  Started[2] = 1;
  Check(Finished[0]);
  Observed = Counter;
  PeerObserved = 1;
  Finished[2] = 1;
  PsTerminateSystemThread(StatusSuccess);
}
static void Thread(void *Context) {
  const U64 Index = (U64)Context;
  KeLeaveCriticalRegion();
  Started[Index] = 1;
  if (Mode == QuantumRemainder || Mode == RemainderBaselineCode) {
    if (!Index && Mode == QuantumRemainder) {
      Check(!KeSetTimerEx(&Timer, -(long long)RemainderDelay, 0, 0));
      Check(KeWaitForSingleObject(&Timer, 0, KernelMode, 0, 0) ==
            StatusSuccess);
    }
  } else if (Mode == LowerRunning && !Index) {
    Check(KeSetPriorityThread(CurrentThread(), LowPriority) == HighPriority);
    Check(Finished[1] && KeQueryPriorityThread(CurrentThread()) == LowPriority);
  } else if ((Mode == WakeHigher || Mode == ReprioritizeWaiting ||
              Mode == MaskedWakeHigher) &&
             !Index) {
    Check(KeWaitForSingleObject(&Event, 0, KernelMode, 0, 0) == StatusSuccess);
    Check(KeQueryPriorityThread(CurrentThread()) == HighPriority);
    if (Mode == WakeHigher)
      Check(!AfterSignal);
    if (Mode == MaskedWakeHigher)
      Check(AfterSignal && !AfterLower);
  } else if (Mode == TimerWakeHigher && !Index) {
    Check(!KeSetTimerEx(&Timer, -(long long)TimerDelay, 0, 0));
    Check(KeWaitForSingleObject(&Timer, 0, KernelMode, 0, 0) == StatusSuccess);
  } else if (Mode == WakeHigher && Index) {
    Check(Started[0] && !Finished[0]);
    KeSetEvent(&Event, 0, 0);
    AfterSignal = 1;
    Check(Finished[0]);
  } else if (Mode == ReprioritizeWaiting && Index) {
    Check(Started[0] && !Finished[0]);
    Check(KeSetPriorityThread(Objects[0], LowPriority) == HighPriority);
    KeSetEvent(&Event, 0, 0);
    AfterSignal = 1;
    Check(!Finished[0]);
    Check(KeSetPriorityThread(Objects[0], HighPriority) == LowPriority);
    Check(Finished[0]);
  } else if (Mode == MaskedWakeHigher && Index) {
    const U8 OldIRQL = KfRaiseIrql(DispatchLevel);
    KeSetEvent(&Event, 0, 0);
    AfterSignal = 1;
    Busy(SmallQuantum);
    Check(!Finished[0]);
    KeLowerIrql(OldIRQL);
    AfterLower = 1;
    Check(Finished[0]);
  } else if (Mode == TimerWakeHigher && Index) {
    // The loop is shorter than one quantum. A timer wake must preempt before
    // quantum expiry, or this thread records a failure before returning.
    for (volatile U32 I = 0; !Finished[0] && I < DeadlineSpinIterations; ++I) {
    }
    Check(Finished[0]);
  } else if (Mode == EqualRoundRobin) {
    while (!Started[1 - Index]) {
    }
    Busy(RoundRobinIterations);
  }
  Finished[Index] = 1;
  PsTerminateSystemThread(StatusSuccess);
}

static NTSTATUS PriorityDispatch(void *Target, IRP *Request) {
  if (Request->Stack->MajorFunction != DispatchIndex)
    return Dispatch(Target, Request);
  Mode = Request->Stack->ControlCode;
  AfterSignal = AfterLower = Ticks = Invalid = 0;
  Counter = Observed = PeerObserved = 0;
  for (U32 I = 0; I < MaxThreads; ++I) {
    Started[I] = Finished[I] = 0;
    Objects[I] = 0;
  }
  void *Handles[MaxThreads] = {0, 0, 0};
  void *Foreground = CurrentThread();
  const int Original = KeSetPriorityThread(Foreground, SetupPriority);
  KeInitializeEvent(&Event, NotificationObject, 0);
  KeInitializeTimer(&Timer);
  OBJECT_ATTRIBUTES Attributes = {sizeof(Attributes), 0, 0,
                                  ObjectKernelHandle, 0, 0};
  const U32 Count = Mode == LowStarvationClock ? 1
                    : Mode == QuantumRemainder || Mode == RemainderBaselineCode
                        ? MaxThreads
                        : 2;
  for (U64 I = 0; I < Count; ++I) {
    void (*Start)(void *) = Thread;
    if (Mode == QuantumRemainder || Mode == RemainderBaselineCode) {
      if (I == 1)
        Start = RemainderCounter;
      if (I == 2)
        Start = RemainderPeer;
    }
    Check(PsCreateSystemThread(&Handles[I], ThreadAllAccess, &Attributes, 0, 0,
                               Start, (void *)I) == StatusSuccess);
    Check(ObReferenceObjectByHandle(Handles[I], 0, 0, KernelMode, &Objects[I],
                                    0) == StatusSuccess);
    Check(KeQueryPriorityThread(Objects[I]) == PriorityDefault);
    int Priority = I ? MediumPriority : HighPriority;
    if (Mode == QueuedHigher || Mode == PromoteReady)
      Priority = I && Mode == QueuedHigher ? HighPriority : LowPriority;
    if (Mode == EqualRoundRobin)
      Priority = HighPriority;
    if (Mode == LowStarvationClock)
      Priority = LowPriority;
    Check(KeSetPriorityThread(Objects[I], Priority) == PriorityDefault);
  }
  if (Mode == LowStarvationClock) {
    KeInitializeDpc(&Dpc, Tick, 0);
    Check(!KeSetTimerEx(&Timer, -(long long)TimerDelay, 0, &Dpc));
    KeSetPriorityThread(Foreground, HighPriority);
    while (!Ticks) {
    }
    Busy(SpinIterations);
    Check(!Started[0] && Ticks == 1);
  } else {
    KeSetPriorityThread(Foreground, Mode == EqualRoundRobin ? LowPriority
                                                            : PriorityDefault);
    if (Mode == QueuedHigher)
      Check(Finished[1] && !Started[0]);
    if (Mode == PromoteReady) {
      Check(!Started[0] && !Started[1]);
      Check(KeSetPriorityThread(Objects[1], HighPriority) == LowPriority);
      Check(Finished[1] && !Started[0]);
    }
    if (Mode == LowerRunning)
      Check(Started[0] && Finished[1] && !Finished[0]);
  }
  for (U32 I = 0; I < Count; ++I) {
    Check(KeWaitForSingleObject(Objects[I], 0, KernelMode, 0, 0) ==
          StatusSuccess);
    Check(Finished[I]);
    ObfDereferenceObject(Objects[I]);
    Check(ZwClose(Handles[I]) == StatusSuccess);
  }
  KeCancelTimer(&Timer);
  Check(KeSetPriorityThread(Foreground, Original) ==
        (Mode == LowStarvationClock ? HighPriority
         : Mode == EqualRoundRobin  ? LowPriority
                                    : PriorityDefault));
  Request->SystemBuffer[0] = Invalid ? FailureMarker : SuccessMarker;
  Request->Status = StatusSuccess;
  Request->Information = OutputLength;
  if (Mode == QuantumRemainder || Mode == RemainderBaselineCode) {
    *(U32 *)(Request->SystemBuffer + OutputLength) = Observed;
    Request->Information = RemainderOutputLength;
  }
  IofCompleteRequest(Request, 0);
  return StatusSuccess;
}

NTSTATUS DriverEntry(DRIVER_OBJECT *Driver, UNICODE_STRING *RegistryPath) {
  NTSTATUS Status = BufferedDriverEntry(Driver, RegistryPath);
  if (Status == StatusSuccess)
    Driver->MajorFunction[DispatchIndex] = (void *)PriorityDispatch;
  return Status;
}

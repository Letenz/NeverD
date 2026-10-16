//===- driver_objects.c - Original object lifetime driver fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
typedef int NTSTATUS;
typedef struct {
  U64 Process, Thread;
} CLIENT_ID;
typedef struct {
  U32 Attributes, Access;
} HANDLE_INFORMATION;
typedef struct {
  _Alignas(8) U8 Bytes[24];
} EVENT;
__declspec(dllimport) void *PsGetCurrentProcess(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) U64 PsGetCurrentThreadId(void);
__declspec(dllimport) U64 PsGetProcessId(void *);
__declspec(dllimport) U64 PsGetThreadId(void *);
__declspec(dllimport) void *PsGetThreadProcess(void *);
__declspec(dllimport) void *IoThreadToProcess(void *);
__declspec(dllimport) U8 PsIsThreadTerminating(void *);
__declspec(dllimport) NTSTATUS PsLookupProcessByProcessId(U64, void **);
__declspec(dllimport) NTSTATUS PsLookupThreadByThreadId(U64, void **);
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) unsigned short ObGetFilterVersion(void);
__declspec(dllimport) void ObfReferenceObject(void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByPointer(void *, U32, void *,
                                                          U8);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(U64, U32, void *, U8,
                                                         void **,
                                                         HANDLE_INFORMATION *);
__declspec(dllimport) NTSTATUS ObOpenObjectByPointer(void *, U32, void *, U32,
                                                     void *, U8, U64 *);
__declspec(dllimport) NTSTATUS PsCreateSystemThread(U64 *, U32, void *, U64,
                                                    CLIENT_ID *,
                                                    void (*)(void *), void *);
__declspec(dllimport) void PsTerminateSystemThread(NTSTATUS);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, U8, U8,
                                                     void *);
__declspec(dllimport) NTSTATUS ZwClose(U64);
__declspec(dllimport) void KeInitializeEvent(EVENT *, U32, U8);
__declspec(dllimport) U32 KeSetEvent(EVENT *, U32, U8);
static void *Process, *Threads[2];
static CLIENT_ID IDs[2];
static EVENT Ready;
static volatile U32 Failure, Completed[2];
#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)0xc000000d;                                             \
  } while (0)

static void worker(void *Context) {
  const U32 Index = (U32)(U64)Context;
  KeWaitForSingleObject(&Ready, 0, 0, 0, 0);
  void *Self = PsGetCurrentThread(), *Lookup = 0;
  if (Self != Threads[Index] || PsGetCurrentThreadId() != IDs[Index].Thread ||
      PsGetThreadProcess(Self) != Process ||
      IoThreadToProcess(Self) != Process || PsIsThreadTerminating(Self) ||
      PsLookupThreadByThreadId(IDs[Index].Thread, &Lookup) || Lookup != Self)
    Failure = 1;
  if (Lookup)
    ObfDereferenceObject(Lookup);
  Completed[Index] = 1;
  PsTerminateSystemThread(0);
}
static void (*volatile Start)(void *) = worker;
static void unload(void *Driver) { (void)Driver; }
static void (*volatile Unload)(void *) = unload;

NTSTATUS DriverEntry(void *Driver, void *Registry) {
  (void)Registry;
  *(void (**)(void *))((U8 *)Driver + 0x68) = Unload;
  void *Lookup = 0, *Current = PsGetCurrentThread();
  Process = PsGetCurrentProcess();
  void *ProcessType = ObGetObjectType(Process);
  void *ThreadType = ObGetObjectType(Current);
  CHECK(ProcessType != ThreadType);
  CHECK(ObGetFilterVersion() == 0x100);
  CHECK(!PsLookupProcessByProcessId(PsGetProcessId(Process), &Lookup));
  CHECK(Lookup == Process);
  ObfDereferenceObject(Lookup);
  CHECK((U32)PsLookupProcessByProcessId(0, &Lookup) == 0xc000000b);
  CHECK((U32)ObReferenceObjectByPointer(Current, 0, ProcessType, 0) ==
        0xc0000024);
  CHECK(!ObReferenceObjectByPointer(Current, 0, ThreadType, 0));
  ObfReferenceObject(Current);
  ObfDereferenceObject(Current);
  ObfDereferenceObject(Current);
  U64 ProcessHandle;
  CHECK(!ObOpenObjectByPointer(Process, 0x200, 0, 0x400, ProcessType, 0,
                               &ProcessHandle));
  HANDLE_INFORMATION Information = {0xffffffff, 0xffffffff};
  CHECK(!ObReferenceObjectByHandle(ProcessHandle, 0x1000, ProcessType, 0,
                                   &Lookup, &Information));
  CHECK(Lookup == Process && Information.Attributes == 0 &&
        Information.Access == 0x400);
  ObfDereferenceObject(Lookup);
  CHECK(!ZwClose(ProcessHandle));

  U64 Handles[2], Copies[2];
  KeInitializeEvent(&Ready, 0, 0);
  for (U32 I = 0; I != 2; ++I) {
    CHECK(!PsCreateSystemThread(&Handles[I], 0x1fffff, 0, 0, &IDs[I], Start,
                                (void *)(U64)I));
    CHECK(IDs[I].Process == 4);
    CHECK(!PsLookupThreadByThreadId(IDs[I].Thread, &Threads[I]));
    CHECK(PsGetThreadId(Threads[I]) == IDs[I].Thread);
    CHECK(ObGetObjectType(Threads[I]) == ThreadType);
    CHECK(!ObOpenObjectByPointer(Threads[I], 0x200, 0, 0x100000, ThreadType, 0,
                                 &Copies[I]));
    CHECK(!ZwClose(Handles[I]));
  }
  CHECK(IDs[0].Thread != IDs[1].Thread);
  KeSetEvent(&Ready, 0, 0);
  for (U32 I = 0; I != 2; ++I) {
    CHECK(!KeWaitForSingleObject(Threads[I], 0, 0, 0, 0));
    CHECK(PsIsThreadTerminating(Threads[I]));
    ObfDereferenceObject(Threads[I]);
    CHECK(!PsLookupThreadByThreadId(IDs[I].Thread, &Lookup));
    CHECK(Lookup == Threads[I]);
    CHECK(!ZwClose(Copies[I]));
    CHECK(PsGetThreadProcess(Lookup) == Process);
    CHECK(IoThreadToProcess(Lookup) == Process);
    ObfDereferenceObject(Lookup);
    CHECK((U32)PsLookupThreadByThreadId(IDs[I].Thread, &Lookup) == 0xc000000b);
  }
  CHECK(!Failure && Completed[0] && Completed[1]);
  return 0;
}

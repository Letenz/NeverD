//===- driver_file_queries.c - Original file query fixture ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Query a retained file from a second file's IOCTL using the real public ABI.

typedef unsigned char U8;
typedef unsigned short U16;
#ifdef NEVERD_WDK_FIXTURE
typedef unsigned long U32;
#else
typedef unsigned int U32;
#endif
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
// Exported byte-result ABI omitted by the pinned WDK headers.
NTKERNELAPI KPROCESSOR_MODE NTAPI PsGetCurrentThreadPreviousMode(VOID);
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PIRP PACKET;
typedef PUNICODE_STRING REGISTRY;
typedef IO_STATUS_BLOCK QUERY_STATUS;
_Static_assert(FIELD_OFFSET(IRP, UserIosb) == 0x48, "x64 query IOSB");
_Static_assert(FIELD_OFFSET(IRP, UserEvent) == 0x50, "x64 query event");
_Static_assert(FIELD_OFFSET(IRP, Tail.Overlay.Thread) == 0x98,
               "x64 issuing thread");
_Static_assert(FIELD_OFFSET(IO_STACK_LOCATION, Parameters.QueryFile.Length) ==
                   8,
               "x64 query length");
_Static_assert(FIELD_OFFSET(IO_STACK_LOCATION,
                            Parameters.QueryFile.FileInformationClass) == 16,
               "x64 query class");
_Static_assert(FIELD_OFFSET(FILE_OBJECT, Event) == 0x98, "x64 file event");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef void *PACKET;
typedef struct {
  union {
    NTSTATUS Status;
    void *Pointer;
  };
  U64 Information;
} QUERY_STATUS;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} *REGISTRY;
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) void IofCompleteRequest(PACKET, U8);
__declspec(dllimport) void ObfReferenceObject(void *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void *PsGetCurrentProcessId(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) U8 ExGetPreviousMode(void);
__declspec(dllimport) char PsGetCurrentThreadPreviousMode(void);
__declspec(dllimport) void *IoThreadToProcess(void *);
__declspec(dllimport) void *PsGetCurrentProcess(void);
__declspec(dllimport) U8 KeAreApcsDisabled(void);
__declspec(dllimport) U8 KeAreAllApcsDisabled(void);
__declspec(dllimport) void KeEnterGuardedRegion(void);
__declspec(dllimport) void KeLeaveGuardedRegion(void);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) void KeInitializeEvent(void *, U32, U8);
__declspec(dllimport) int KeSetEvent(void *, int, U8);
__declspec(dllimport) NTSTATUS KeWaitForSingleObject(void *, U32, U8, U8,
                                                     void *);
__declspec(dllimport) DEVICE IoGetRelatedDeviceObject(void *);
__declspec(dllimport) DEVICE IoGetBaseFileSystemDeviceObject(void *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(REGISTRY);
__declspec(dllimport) NTSTATUS IoQueryFileInformation(void *, U32, U32, void *,
                                                      U32 *);
__declspec(dllimport) NTSTATUS ZwQueryInformationFile(void *, QUERY_STATUS *,
                                                      void *, U32, U32);
__declspec(dllimport) NTSTATUS ObOpenObjectByPointer(void *, U32, void *, U32,
                                                     void *, U8, void **);
__declspec(dllimport) NTSTATUS ZwClose(void *);
#endif

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0050000u + __LINE__);                               \
  } while (0)

static DEVICE Device;
static void *File, *CallerThread, *Work;
static PACKET Pending;
static U8 *Buffer;
static U32 *Returned;
static U32 Creates, Queries, Closes, Failed, ActiveClass, InQuery;
static U16 Mode;
static U8 Native;
static QUERY_STATUS *NativeStatus;
static U64 Done[3];

static NTSTATUS complete(PACKET Irp, NTSTATUS Status, U64 Information) {
  *(U32 *)((U8 *)Irp + 0x30) = (U32)Status;
  *(U64 *)((U8 *)Irp + 0x38) = Information;
  IofCompleteRequest(Irp, 0);
  return Status;
}

static U32 queryStatus(void) {
  if (Mode == 'W')
    return 0x80000005;
  if (Mode == 'E' || Mode == 'M' || Mode == 'C')
    return 0xc0000001;
  return 0;
}

static void worker(DEVICE Owner, void *Context) {
  if (Owner != Device || Context != File ||
      PsGetCurrentThread() == CallerThread || (U64)PsGetCurrentProcessId() != 4)
    Failed = 1;
  U8 *Stack = *(U8 **)((U8 *)Pending + 0xb8);
  U8 Major = Stack[0];
  IoFreeWorkItem(Work);
  complete(Pending,
           Failed       ? (NTSTATUS)0xc005ffff
           : Major == 5 ? (NTSTATUS)queryStatus()
                        : 0,
           Major == 5 ? 7 : 0);
  if (Mode == 'B')
    KeSetEvent((void *)Done, 0, 0);
}

static NTSTATUS dispatchQuery(PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  U8 Async = !(*(U32 *)((U8 *)File + 0x50) & 2);
  CHECK(InQuery && *(void **)(Stack + 0x30) == File);
  CHECK(*(void **)((U8 *)Irp + 0xc0) == File);
  CHECK(*(void **)((U8 *)Irp + 0x98) == CallerThread);
  CHECK(PsGetCurrentThread() == CallerThread);
  CHECK(KeAreApcsDisabled() == !Async && !KeAreAllApcsDisabled());
  CHECK((U64)PsGetCurrentProcessId() == 0x2000 && ExGetPreviousMode() == 1);
  CHECK(PsGetCurrentThreadPreviousMode() == 1);
  CHECK(IoThreadToProcess(CallerThread) == PsGetCurrentProcess());
  CHECK(*(U32 *)((U8 *)Irp + 0x10) ==
        ((Async ? 0x14u : 0x10u) | (Native ? 0x860u : 0u)));
  CHECK(*((U8 *)Irp + 0x47) == (Async ? 0 : 2));
  CHECK(*((U8 *)Irp + 0x40) == 0);
  U8 *System = *(U8 **)((U8 *)Irp + 0x18);
  CHECK((System == Buffer) == !Native);
  if (Native) {
    CHECK(*(void **)((U8 *)Irp + 0x70) == Buffer);
    for (U32 I = 0; I < 32; ++I)
      CHECK(System[I] == 0);
  }
  CHECK(*(void **)((U8 *)Irp + 0x48));
  CHECK((*(void **)((U8 *)Irp + 0x50) != 0) == Async);
  CHECK(*(U32 *)(Stack + 8) == 32);
  CHECK(*(U32 *)(Stack + 16) == (ActiveClass == 75 ? 71 : ActiveClass));
  CHECK(Stack[2] == (ActiveClass == 75 ? 1 : 0));
  for (U32 I = 0; I < 7; ++I)
    System[I] = (U8)(0x20 + I);
  ++Queries;
  if (Mode == 'C' && Queries == 3)
    ObfDereferenceObject(File);
  void *IOSB = *(void **)((U8 *)Irp + 0x48);
  if (Mode == 'P' || Mode == 'W' || Mode == 'C' || Mode == 'B' || Mode == 'U') {
    Pending = Irp;
    Work = IoAllocateWorkItem(Device);
    CHECK(Work);
    Stack[3] |= 1;
    if (Mode == 'B')
      KeInitializeEvent((void *)Done, 0, 0);
    IoQueueWorkItem(Work, worker, 1, File);
    if (Mode == 'B') {
      CHECK(!KeWaitForSingleObject((void *)Done, 0, 0, 0, 0));
      CHECK(*(U64 *)((U8 *)IOSB + 8) == 7);
      return 0x103;
    }
    return 0x103;
  }
  U8 PreviousIRQL = 0;
  if (Mode == 'G')
    KeEnterGuardedRegion();
  if (Mode == 'L') {
#ifdef NEVERD_WDK_FIXTURE
    KeRaiseIrql(DISPATCH_LEVEL, &PreviousIRQL);
#else
    PreviousIRQL = KfRaiseIrql(2);
#endif
  }
  const U32 Status = queryStatus();
  complete(Irp, (NTSTATUS)Status, 7);
  if (Mode == 'G' || Mode == 'L') {
    if (!Native)
      CHECK(*(U64 *)((U8 *)IOSB + 8) == 0);
    if (Mode == 'G')
      KeLeaveGuardedRegion();
    else
      KeLowerIrql(PreviousIRQL);
  }
  if (Native) {
    CHECK(NativeStatus->Information == 0xa5a5a5a5a5a5a5a5ull);
    CHECK(Buffer[0] == 0xa5);
    // Deferred native completion retains its system allocation until the
    // dispatch returns. Borrowed queries keep their caller allocation too.
    CHECK(*(U32 *)((U8 *)Irp + 0x30) == Status);
  } else {
    CHECK(*(U64 *)((U8 *)IOSB + 8) == (Status ? 0 : 7));
  }
  System[30] = 0x5a;
  return Mode == 'M' ? 0 : (NTSTATUS)Status;
}

static NTSTATUS handle(DEVICE Owner, PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  void *CurrentFile = *(void **)(Stack + 0x30);
  CHECK(Owner == Device);
  if (Stack[0] == 0) {
    if (++Creates == 1) {
      File = CurrentFile;
      ObfReferenceObject(File);
    }
  } else if (Stack[0] == 2) {
    ++Closes;
    if (CurrentFile == File && !Native) {
      CHECK(PsGetCurrentThread() == CallerThread);
      CHECK((U64)PsGetCurrentProcessId() == 0x2000);
      if (Mode == 'C') {
        CHECK(InQuery && *Returned == 0xdeadbeef);
        CHECK(KeAreApcsDisabled() && !KeAreAllApcsDisabled());
        Pending = Irp;
        Work = IoAllocateWorkItem(Device);
        CHECK(Work);
        Stack[3] |= 1;
        IoQueueWorkItem(Work, worker, 1, File);
        return 0x103;
      }
    }
  } else if (Stack[0] == 14) {
    CHECK(CurrentFile != File && Creates == 2);
    CHECK(IoGetRelatedDeviceObject(File) == Device);
    CHECK(IoGetBaseFileSystemDeviceObject(File) == Device);
    static U16 BaseName[] = {'I', 'o', 'G', 'e', 't', 'B', 'a', 's',
                             'e', 'F', 'i', 'l', 'e', 'S', 'y', 's',
                             't', 'e', 'm', 'D', 'e', 'v', 'i', 'c',
                             'e', 'O', 'b', 'j', 'e', 'c', 't', 0};
    struct {
      U16 Length, MaximumLength;
      U16 *Buffer;
    } BaseRoutine = {sizeof(BaseName) - 2, sizeof(BaseName), BaseName};
    typedef DEVICE (*BASE_QUERY)(void *);
    BASE_QUERY BaseQuery =
        (BASE_QUERY)MmGetSystemRoutineAddress((REGISTRY)&BaseRoutine);
    CHECK(BaseQuery && BaseQuery(File) == Device);
    CallerThread = PsGetCurrentThread();
    void *Handle = 0;
    if (Native)
      CHECK(!ObOpenObjectByPointer(File, 0x200, 0, 0, 0, 0, &Handle));
    for (U32 I = 0; I < 3; ++I) {
      U64 Storage[8];
      U64 Length = 0xaabbccdddeadbeefull;
      for (U32 J = 0; J < 8; ++J)
        Storage[J] = 0xa5a5a5a5a5a5a5a5ull;
      Buffer = Mode == 'U' ? *(U8 **)((U8 *)Irp + 0x70) : (U8 *)Storage;
      if (Mode == 'U')
        for (U32 J = 0; J < 64; ++J)
          Buffer[J] = 0xa5;
      Returned = !Native && I == 1 ? (U32 *)(Buffer + 4) : (U32 *)&Length;
      *Returned = 0xdeadbeef;
      ActiveClass = I == 1 ? 75 : I == 2 ? (Native ? 6 : 14) : 4;
      QUERY_STATUS StatusBlock;
      NativeStatus = I == 1 ? (QUERY_STATUS *)(Buffer + 8) : &StatusBlock;
      if (Native) {
        NativeStatus->Pointer = (void *)0xa5a5a5a5a5a5a5a5ull;
        NativeStatus->Information = 0xa5a5a5a5a5a5a5a5ull;
      }
      InQuery = 1;
      const U32 Status =
          Native ? (U32)ZwQueryInformationFile(Handle, NativeStatus, Buffer, 32,
                                               ActiveClass)
                 : (U32)IoQueryFileInformation(File, ActiveClass, 32, Buffer,
                                               Returned);
      InQuery = 0;
      CHECK(Status == (Mode == 'M' ? 0 : queryStatus()));
      if (Native) {
        U8 Published = Mode != 'E' && Mode != 'M';
        CHECK(NativeStatus->Information ==
              (Published ? 7 : 0xa5a5a5a5a5a5a5a5ull));
        CHECK((U32)NativeStatus->Status ==
              (Published ? queryStatus() : 0xa5a5a5a5u));
      } else {
        CHECK(*Returned == ((Mode == 'E' || Mode == 'M') ? 0u : 7u));
      }
      CHECK((Length >> 32) == 0xaabbccdd);
      U8 Copied = !Native || (Mode != 'E' && Mode != 'M');
      CHECK(Buffer[0] == (Copied ? 0x20 : 0xa5) &&
            Buffer[3] == (Copied ? 0x23 : 0xa5) && Buffer[31] == 0xa5);
      CHECK(PsGetCurrentThread() == CallerThread);
      CHECK(!KeAreApcsDisabled() && !KeAreAllApcsDisabled());
      CHECK((U64)PsGetCurrentProcessId() == 0x2000 && ExGetPreviousMode() == 1);
      CHECK(PsGetCurrentThreadPreviousMode() == 1);
      CHECK(IoThreadToProcess(CallerThread) == PsGetCurrentProcess());
      CHECK(!Failed);
      Buffer[0] = 0x7f;
      CHECK(Buffer[0] == 0x7f);
    }
    if (Mode != 'C')
      ObfDereferenceObject(File);
    if (Native)
      CHECK(!ZwClose(Handle));
    CHECK(Closes == (Native ? 0 : 1));
  } else {
    CHECK(Stack[0] == 18);
  }
  return 0;
}

static NTSTATUS dispatch(DEVICE Owner, PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  if (Stack[0] == 5)
    return dispatchQuery(Irp);
  NTSTATUS Status = handle(Owner, Irp);
  return (U32)Status == 0x103 ? Status : complete(Irp, Status, 0);
}
static NTSTATUS (*volatile Dispatch)(DEVICE, PACKET) = dispatch;
static void unload(DRIVER Driver) {
  (void)Driver;
  if (Creates == 2 && Queries == 3 && Closes == 2 && !Failed)
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;
NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  Mode = Registry->Length ? Registry->Buffer[Registry->Length / 2 - 1] : 'S';
  Native = Registry->Length >= 4 &&
           Registry->Buffer[Registry->Length / 2 - 2] == 'N';
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  const U32 Majors[] = {0, 2, 5, 14, 18};
  for (U32 I = 0; I < sizeof(Majors) / sizeof(Majors[0]); ++I)
    *(NTSTATUS(**)(DEVICE, PACKET))((U8 *)Driver + 0x70 + 8 * Majors[I]) =
        Dispatch;
  return 0;
}

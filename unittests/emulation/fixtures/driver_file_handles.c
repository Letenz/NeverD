//===- driver_file_handles.c - Original file handle fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise per-process unlock, last-handle cleanup and final body deletion.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
_Static_assert(sizeof(OBJECT_HANDLE_INFORMATION) == 8,
               "x64 object handle information ABI");
// Exported by the target kernel; this WDK omits its declaration.
NTKERNELAPI POBJECT_TYPE ObGetObjectType(PVOID Object);
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PIRP PACKET;
typedef PUNICODE_STRING REGISTRY;
#define FILE_TYPE (*IoFileObjectType)
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef void *PACKET;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} *REGISTRY;
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) void IofCompleteRequest(PACKET, U8);
__declspec(dllimport) NTSTATUS ObReferenceObjectByPointer(void *, U32, void *,
                                                          U8);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) extern void *IoFileObjectType;
#define FILE_TYPE IoFileObjectType
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void *PsGetCurrentProcessId(void);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) U8 ExGetPreviousMode(void);
__declspec(dllimport) NTSTATUS ObOpenObjectByPointer(void *, U32, void *, U32,
                                                     void *, U8, void **);
__declspec(dllimport) NTSTATUS ObReferenceObjectByHandle(void *, U32, void *,
                                                         U8, void **, void *);
__declspec(dllimport) NTSTATUS ZwClose(void *);
__declspec(dllimport) void *IoGetCurrentProcess(void);
__declspec(dllimport) void *PsGetProcessId(void *);
#endif

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0040000u + __LINE__);                               \
  } while (0)
static DEVICE Device;
static void *OldFile, *Handle, *ReleaseThread, *AsyncWork, *ReleaseWork;
static PACKET Pending, ReleasePacket;
static U32 Creates, Cleanups, Closes, Unlocks, Controls;
static U32 InRelease, AsyncCleanup, AsyncClose, Failed;
static U16 Mode;

static NTSTATUS complete(PACKET Irp, NTSTATUS Status) {
  *(U32 *)((U8 *)Irp + 0x30) = (U32)Status;
  *(U64 *)((U8 *)Irp + 0x38) = 0;
  IofCompleteRequest(Irp, 0);
  return Status;
}

static void completionWorker(DEVICE Owner, void *Context) {
  U8 *Stack = *(U8 **)((U8 *)Pending + 0xb8);
  if (Owner != Device || Context != OldFile ||
      PsGetCurrentThread() == ReleaseThread ||
      (U64)PsGetProcessId(IoGetCurrentProcess()) != 4)
    Failed = 1;
  if (Stack[0] == 18)
    ++AsyncCleanup;
  else if (Stack[0] == 2)
    ++AsyncClose;
  else
    Failed = 1;
  IoFreeWorkItem(AsyncWork);
  complete(Pending, Failed ? (NTSTATUS)0xc004ffff : 0);
}

static NTSTATUS release(void) {
  ReleaseThread = PsGetCurrentThread();
  InRelease = 1;
  CHECK(!ZwClose(Handle));
  CHECK(Cleanups == 1 && Closes == 1 && !Failed);
  CHECK((U64)PsGetProcessId(IoGetCurrentProcess()) ==
        (Mode == 'W' ? 4 : 0x2000));
  CHECK(PsGetCurrentThread() == ReleaseThread);
  CHECK(*(U32 *)((U8 *)Device + 4) == 1);
  if (Mode == 'P' || Mode == 'B')
    CHECK(AsyncCleanup == 1);
  if (Mode == 'C' || Mode == 'B')
    CHECK(AsyncClose == 1);
  InRelease = 0;
  return 0;
}

static void releaseWorker(DEVICE Owner, void *Context) {
  NTSTATUS Status = release();
  if (Owner != Device || Context != OldFile)
    Status = (NTSTATUS)0xc004fffe;
  IoFreeWorkItem(ReleaseWork);
  complete(ReleasePacket, Status);
}

static NTSTATUS handle(DEVICE Owner, PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  void *File = *(void **)(Stack + 0x30);
  const U8 Major = Stack[0];
  CHECK(Owner == Device && File == *(void **)((U8 *)Irp + 0xc0));
  CHECK(ObGetObjectType(File) == FILE_TYPE);
  if (Major == 0) {
    *(U64 *)((U8 *)File + 0x18) = ++Creates;
    if (Creates == 1) {
      OldFile = File;
      CHECK(!ObReferenceObjectByPointer(File, 0, FILE_TYPE, 0));
    }
  } else if (Major == 17) {
    CHECK(File == OldFile && Stack[1] == 3 && Unlocks == 0);
    CHECK(!*((U8 *)Irp + 0x40) && *(U32 *)((U8 *)Irp + 0x10) == 0x1004);
    CHECK((U64)PsGetCurrentProcessId() == 0x1000);
    CHECK((U64)PsGetProcessId(IoGetCurrentProcess()) == 0x1000);
    ++Unlocks;
  } else if (Major == 18 || Major == 2) {
    CHECK(!*((U8 *)Irp + 0x40) && *(U32 *)((U8 *)Irp + 0x10) == 0x404);
    if (File == OldFile) {
      CHECK(InRelease && PsGetCurrentThread() == ReleaseThread);
      CHECK((U64)PsGetCurrentProcessId() == (Mode == 'W' ? 4 : 0x2000));
      CHECK((U64)PsGetProcessId(IoGetCurrentProcess()) ==
            (Major == 18 || Mode == 'W' ? 4 : 0x2000));
      CHECK(ExGetPreviousMode() == (Mode == 'W' ? 0 : 1));
      CHECK(*(U64 *)((U8 *)File + 0x18) == 1);
      if (Major == 18)
        ++Cleanups;
      else {
        ++Closes;
        if (Mode == 'P' || Mode == 'B')
          CHECK(AsyncCleanup == 1);
      }
      if ((Major == 18 && (Mode == 'P' || Mode == 'B')) ||
          (Major == 2 && (Mode == 'C' || Mode == 'B'))) {
        Pending = Irp;
        AsyncWork = IoAllocateWorkItem(Device);
        CHECK(AsyncWork);
        Stack[3] |= 1;
        IoQueueWorkItem(AsyncWork, completionWorker, 1, File);
        return 0x103;
      }
    }
  } else if (Major == 14) {
    if (++Controls == 1) {
      void *First, *Referenced;
      U32 Info[2];
      CHECK(File == OldFile && Creates == 1);
      CHECK(!ObOpenObjectByPointer(File, 0x200, 0, 0x80000000, FILE_TYPE, 0,
                                   &First));
      CHECK(!ObOpenObjectByPointer(File, 0x200, 0, 0x40000000, FILE_TYPE, 0,
                                   &Handle));
      CHECK(!ObReferenceObjectByHandle(First, 1, FILE_TYPE, 0, &Referenced,
                                       (void *)Info));
      CHECK(Referenced == OldFile && Info[0] == 0 && Info[1] == 0x120089);
      ObfDereferenceObject(Referenced);
      CHECK(!ZwClose(First));
      CHECK(!Unlocks && !Cleanups && !Closes);
      ObfDereferenceObject(OldFile);
    } else {
      CHECK(File != OldFile && Creates == 2 && Unlocks == 1);
      CHECK(!Cleanups && !Closes);
      CHECK(*(U32 *)((U8 *)Device + 4) == 2);
      if (Mode == 'L')
        return 0;
      if (Mode == 'W') {
        ReleasePacket = Irp;
        ReleaseWork = IoAllocateWorkItem(Device);
        CHECK(ReleaseWork);
        Stack[3] |= 1;
        IoQueueWorkItem(ReleaseWork, releaseWorker, 1, OldFile);
        return 0x103;
      }
      return release();
    }
  } else
    CHECK(0);
  return 0;
}

static NTSTATUS dispatch(DEVICE Owner, PACKET Irp) {
  NTSTATUS Status = handle(Owner, Irp);
  if ((U32)Status != 0x103)
    complete(Irp, Status);
  return Status;
}
static NTSTATUS (*volatile Dispatch)(DEVICE, PACKET) = dispatch;
static void unload(DRIVER Driver) {
  (void)Driver;
  if (!Failed && Creates == 2 && Controls == 2 && Unlocks == 1 &&
      Cleanups == 1 && Closes == 1)
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;
NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  const U32 Length = Registry->Length / 2;
  Mode = Length ? Registry->Buffer[Length - 1] : 'S';
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  const U32 Majors[] = {0, 2, 14, 17, 18};
  for (U32 I = 0; I < sizeof(Majors) / sizeof(Majors[0]); ++I)
    *(NTSTATUS(**)(DEVICE, PACKET))((U8 *)Driver + 0x70 + 8 * Majors[I]) =
        Dispatch;
  return 0;
}

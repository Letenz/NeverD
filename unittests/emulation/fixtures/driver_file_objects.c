//===- driver_file_objects.c - Original retained file fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep an old FILE_OBJECT across CLOSE and reuse of its scenario identity.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
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
#endif

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0030000u + __LINE__);                               \
  } while (0)
static DEVICE Device;
static void *OldFile, *Work, *ReleaseThread;
static PACKET Pending;
static U32 Creates, Cleanups, Closes, InRelease, Finished, Failed;
static U16 Mode;

static NTSTATUS complete(PACKET Irp, NTSTATUS Status) {
  *(U32 *)((U8 *)Irp + 0x30) = (U32)Status;
  *(U64 *)((U8 *)Irp + 0x38) = 0;
  IofCompleteRequest(Irp, 0);
  return Status;
}

static void worker(DEVICE Owner, void *Context) {
  if (Owner != Device || Context != OldFile ||
      *(U64 *)((U8 *)Context + 0x18) != 1 || *(U32 *)((U8 *)Owner + 4) != 2)
    Failed = 1;
  Finished = 1;
  IoFreeWorkItem(Work);
  complete(Pending, Failed ? (NTSTATUS)0xc003ffff : 0);
}

static void releaseWorker(DEVICE Owner, void *Context) {
  ReleaseThread = PsGetCurrentThread();
  InRelease = 1;
  ObfDereferenceObject(Context);
  InRelease = 0;
  if (Owner != Device || Closes != 1 || *(U32 *)((U8 *)Owner + 4) != 1)
    Failed = 1;
  IoFreeWorkItem(Work);
  complete(Pending, Failed ? (NTSTATUS)0xc003fffe : 0);
}

static NTSTATUS handle(DEVICE Owner, PACKET Irp) {
  U8 *Stack = *(U8 **)((U8 *)Irp + 0xb8);
  void *File = *(void **)(Stack + 0x30);
  const U8 Major = Stack[0];
  CHECK(Owner == Device);
  CHECK(File == *(void **)((U8 *)Irp + 0xc0));
  CHECK(ObGetObjectType(File) == FILE_TYPE);
  if (Major == 0) {
    *(U64 *)((U8 *)File + 0x18) = ++Creates;
    if (Creates == 1) {
      OldFile = File;
      CHECK(!ObReferenceObjectByPointer(File, 0, FILE_TYPE, 0));
      if (Mode == 'F')
        return (NTSTATUS)0xc0000001;
    }
  } else if (Major == 18) {
    CHECK(!*((U8 *)Irp + 0x40));
    ++Cleanups;
  } else if (Major == 2) {
    CHECK(!*((U8 *)Irp + 0x40));
    if (File == OldFile) {
      CHECK(InRelease);
      CHECK(PsGetCurrentThread() == ReleaseThread);
      CHECK(ExGetPreviousMode() == (Mode == 'W' ? 0 : 1));
      CHECK((U64)PsGetCurrentProcessId() == (Mode == 'W' ? 4 : 0x2000));
      CHECK(*(U64 *)((U8 *)File + 0x18) == 1);
      ++Closes;
      if (Mode == 'P') {
        Pending = Irp;
        Work = IoAllocateWorkItem(Device);
        CHECK(Work);
        Stack[3] |= 1;
        IoQueueWorkItem(Work, worker, 1, File);
        return 0x103;
      }
    } else {
      CHECK(*(U64 *)((U8 *)File + 0x18) == 2);
      ++Closes;
    }
  } else if (Major == 14) {
    CHECK(File != OldFile && Closes == 0);
    CHECK(*(U64 *)((U8 *)File + 0x18) == 2);
    CHECK(*(U64 *)((U8 *)OldFile + 0x18) == 1);
    CHECK(*(void **)((U8 *)OldFile + 8) == (Mode == 'F' ? 0 : Device));
    CHECK((U64)PsGetCurrentProcessId() == 0x2000);
    if (Mode == 'W') {
      Pending = Irp;
      Work = IoAllocateWorkItem(Device);
      CHECK(Work);
      Stack[3] |= 1;
      IoQueueWorkItem(Work, releaseWorker, 1, OldFile);
      return 0x103;
    }
    if (Mode != 'L') {
      ReleaseThread = PsGetCurrentThread();
      InRelease = 1;
      ObfDereferenceObject(OldFile);
      InRelease = 0;
      CHECK(Closes == (Mode == 'F' ? 0 : 1));
      CHECK(Mode != 'P' || Finished);
      CHECK(*(U32 *)((U8 *)Device + 4) == 1);
    }
  } else {
    CHECK(0);
  }
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
  if (!Failed && Creates == 2 && Cleanups == (Mode == 'F' ? 1 : 2) &&
      Closes == (Mode == 'F' ? 1 : 2))
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;
NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  const U32 Length = Registry->Length / 2;
  Mode = Length ? Registry->Buffer[Length - 1] : 'S';
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  const U32 Majors[] = {0, 2, 14, 18};
  for (U32 I = 0; I < sizeof(Majors) / sizeof(Majors[0]); ++I)
    *(NTSTATUS(**)(DEVICE, PACKET))((U8 *)Driver + 0x70 + 8 * Majors[I]) =
        Dispatch;
  return 0;
}

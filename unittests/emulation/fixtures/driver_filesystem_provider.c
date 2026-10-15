//===- driver_filesystem_provider.c - Original volume identity fixture ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe declared devices in entry, worker and unload without file I/O.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long U32;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
// Target exports whose declarations are omitted by this WDK.
NTKERNELAPI POBJECT_TYPE ObGetObjectType(PVOID Object);
NTKERNELAPI NTSTATUS ObReferenceObjectByName(
    PUNICODE_STRING Name, ULONG Attributes, PACCESS_STATE AccessState,
    ACCESS_MASK DesiredAccess, POBJECT_TYPE Type, KPROCESSOR_MODE Mode,
    PVOID ParseContext, PVOID *Object);
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
typedef UNICODE_STRING TEXT;
_Static_assert(FIELD_OFFSET(DEVICE_OBJECT, AlignmentRequirement) == 0x98,
               "device alignment ABI");
_Static_assert(FIELD_OFFSET(DEVICE_OBJECT, SectorSize) == 0x130,
               "device sector ABI");
_Static_assert(FIELD_OFFSET(DEVICE_OBJECT, DeviceType) == 0x48,
               "device type ABI");
_Static_assert(FIELD_OFFSET(DRIVER_OBJECT, DriverUnload) == 0x68,
               "driver unload ABI");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} TEXT, *REGISTRY;
__declspec(dllimport) void *ObGetObjectType(void *);
__declspec(dllimport) NTSTATUS ObReferenceObjectByName(TEXT *, U32, void *, U32,
                                                       void *, U8, void *,
                                                       void **);
__declspec(dllimport) void RtlInitUnicodeString(TEXT *, const U16 *);
__declspec(dllimport) void ObfDereferenceObject(void *);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) DEVICE IoAttachDeviceToDeviceStack(DEVICE, DEVICE);
__declspec(dllimport) void IoDetachDevice(DEVICE);
__declspec(dllimport) DEVICE IoGetAttachedDeviceReference(DEVICE);
__declspec(dllimport) DEVICE IoGetDeviceAttachmentBaseRef(DEVICE);
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
#endif
#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0080000u + __LINE__);                               \
  } while (0)
static DEVICE Guest, Volume;
static DRIVER Owner;
static void *Work;
static volatile U32 Done, Failed;
static U16 Mode;

static NTSTATUS referenceVolume(DEVICE *Result) {
  TEXT Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"\\Device\\DeclaredVolume");
  return ObReferenceObjectByName(&Name, 0x40, 0, 0, ObGetObjectType(Guest), 0,
                                 0, (void **)Result);
}
static NTSTATUS probe(void) {
  DEVICE Observed = 0;
  CHECK(!referenceVolume(&Observed) && Observed == Volume);
  CHECK(*(U16 *)Volume == 3);
  CHECK(*(DRIVER *)((U8 *)Volume + 8) != Owner);
  CHECK(*(U32 *)((U8 *)Volume + 0x30) == 0);
  CHECK(*(U32 *)((U8 *)Volume + 0x48) == 8);
  CHECK(*(U32 *)((U8 *)Volume + 0x98) == 511);
  CHECK(*(U16 *)((U8 *)Volume + 0x130) == 4096);
  CHECK(ObGetObjectType(Guest) == ObGetObjectType(Volume));
  ObfDereferenceObject(Observed);
  CHECK(IoGetAttachedDeviceReference(Volume) == Volume);
  ObfDereferenceObject(Volume);
  CHECK(IoGetDeviceAttachmentBaseRef(Volume) == Volume);
  ObfDereferenceObject(Volume);
  return 0;
}
static void worker(DEVICE Device, void *Context) {
  Failed = Device != Guest || Context != Volume || probe();
  IoFreeWorkItem(Work);
  Done = 1;
}
static void unload(DRIVER Driver) {
  if (!Guest)
    return;
  if (Driver == Owner && Done && !Failed && !probe() && Mode != 'G')
    IoDeleteDevice(Guest);
}
static void (*volatile Unload)(DRIVER) = unload;
NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  Owner = Driver;
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  Mode = Registry->Length ? Registry->Buffer[Registry->Length / 2 - 1] : 0;
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Guest));
  const NTSTATUS Status = referenceVolume(&Volume);
  if (Mode != 'S' && Mode != 'L' && Mode != 'G' && Mode != 'F' && Mode != 'W') {
    CHECK((U32)Status == 0xc0000034u && !Volume);
    IoDeleteDevice(Guest);
    Guest = 0;
    return 0;
  }
  CHECK(!Status && Volume);
  ObfDereferenceObject(Volume);
  CHECK(!probe());
  CHECK(*(DEVICE *)((U8 *)Driver + 8) == Guest);
  CHECK(!*(DEVICE *)((U8 *)Guest + 0x10));
  if (Mode == 'W') {
    *(volatile U32 *)((U8 *)Volume + 0x98) = 0;
    return (NTSTATUS)0xc008ffffu;
  }
  CHECK(IoAttachDeviceToDeviceStack(Guest, Volume) == Volume);
  CHECK(*(U32 *)((U8 *)Guest + 0x98) == 511);
  CHECK(IoGetDeviceAttachmentBaseRef(Guest) == Volume);
  ObfDereferenceObject(Volume);
  if (Mode == 'F')
    return 0;
  IoDetachDevice(Volume);
  if (Mode == 'L')
    CHECK(IoGetAttachedDeviceReference(Volume) == Volume);
  Work = IoAllocateWorkItem(Guest);
  CHECK(Work);
  IoQueueWorkItem(Work, worker, 1, Volume);
  return 0;
}

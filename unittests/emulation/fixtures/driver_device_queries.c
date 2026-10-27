//===- driver_device_queries.c - Original device query fixture ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise attachment queries and their shared pointer-reference lifetimes.
//===----------------------------------------------------------------------===//

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntifs.h>
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
_Static_assert(__builtin_offsetof(DRIVER_OBJECT, DriverUnload) == 0x68,
               "driver unload ABI");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} *REGISTRY;
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) DEVICE IoAttachDeviceToDeviceStack(DEVICE, DEVICE);
__declspec(dllimport) void IoDetachDevice(DEVICE);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) DEVICE IoGetAttachedDevice(DEVICE);
__declspec(dllimport) DEVICE IoGetAttachedDeviceReference(DEVICE);
__declspec(dllimport) DEVICE IoGetLowerDeviceObject(DEVICE);
__declspec(dllimport) DEVICE IoGetDeviceAttachmentBaseRef(DEVICE);
__declspec(dllimport) void ObfDereferenceObject(void *);
#endif

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0020000u + __LINE__);                               \
  } while (0)

static void unload(DRIVER Driver) { (void)Driver; }
static void (*volatile Unload)(DRIVER) = unload;

NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  DEVICE Devices[3];
  for (U32 I = 0; I != 3; ++I)
    CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Devices[I]));
  DEVICE Lower = Devices[0], Middle = Devices[1], Upper = Devices[2];
  CHECK(IoAttachDeviceToDeviceStack(Middle, Lower) == Lower);
  CHECK(IoAttachDeviceToDeviceStack(Upper, Lower) == Middle);
  for (U32 I = 0; I != 3; ++I) {
    CHECK(IoGetAttachedDevice(Devices[I]) == Upper);
    CHECK(IoGetAttachedDeviceReference(Devices[I]) == Upper);
    ObfDereferenceObject(Upper);
    CHECK(IoGetDeviceAttachmentBaseRef(Devices[I]) == Lower);
    ObfDereferenceObject(Lower);
    CHECK(*(U32 *)((U8 *)Devices[I] + 4) == 0);
  }
  CHECK(!IoGetLowerDeviceObject(Lower));
  CHECK(IoGetLowerDeviceObject(Upper) == Middle);
  CHECK(IoGetDeviceAttachmentBaseRef(Upper) == Lower);
  CHECK(IoGetAttachedDeviceReference(Lower) == Upper);
  IoDeleteDevice(Lower);
  CHECK(IoGetLowerDeviceObject(Middle) == Lower);
  ObfDereferenceObject(Lower);
  IoDeleteDevice(Upper);
  CHECK(!IoGetLowerDeviceObject(Upper));
  CHECK(IoGetAttachedDevice(Lower) == Upper);
  IoDeleteDevice(Middle);
  CHECK(!IoGetLowerDeviceObject(Middle));
  IoDetachDevice(Middle);
  IoDetachDevice(Lower);
  CHECK(!*(DEVICE *)((U8 *)Driver + 8));
  for (U32 I = 0; I != 3; ++I) {
    CHECK(*(U16 *)Devices[I] == 3);
    CHECK(IoGetAttachedDevice(Devices[I]) == Devices[I]);
    CHECK(IoGetDeviceAttachmentBaseRef(Devices[I]) == Devices[I]);
    ObfDereferenceObject(Devices[I]);
  }
  ObfDereferenceObject(Lower);
  ObfDereferenceObject(Middle);
  const U32 Characters = Registry->Length / sizeof(U16);
  if (!Characters || Registry->Buffer[Characters - 1] != 'L')
    ObfDereferenceObject(Upper);
  return 0;
}

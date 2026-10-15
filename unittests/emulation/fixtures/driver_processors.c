//===- driver_processors.c - Original processor and IRQL ABI fixture ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise processor output and paired IRQL through imports and lookup.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntddk.h>
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
typedef PKDPC DPC;
typedef PROCESSOR_NUMBER NUMBER;
typedef UNICODE_STRING ROUTINE_NAME;
_Static_assert(sizeof(NUMBER) == 4, "processor record width");
_Static_assert(FIELD_OFFSET(NUMBER, Group) == 0, "group offset");
_Static_assert(FIELD_OFFSET(NUMBER, Number) == 2, "number offset");
_Static_assert(FIELD_OFFSET(NUMBER, Reserved) == 3, "reserved offset");
_Static_assert(sizeof(KDPC) == 64, "DPC storage");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef void *DPC;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} ROUTINE_NAME, *REGISTRY;
typedef struct {
  U16 Group;
  U8 Number, Reserved;
} NUMBER;
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) U64 KeQueryActiveProcessors(void);
__declspec(dllimport) U32 KeGetCurrentProcessorNumberEx(NUMBER *);
__declspec(dllimport) U8 KeRaiseIrqlToDpcLevel(void);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) void KeLowerIrql(U8);
__declspec(dllimport) void *MmGetSystemRoutineAddress(ROUTINE_NAME *);
__declspec(dllimport) void RtlInitUnicodeString(ROUTINE_NAME *, const U16 *);
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
__declspec(dllimport) void
KeInitializeDpc(DPC, void (*)(DPC, void *, void *, void *), void *);
__declspec(dllimport) U8 KeInsertQueueDpc(DPC, void *, void *);
#endif

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0060000u + __LINE__);                               \
  } while (0)

// The genuine build derives both dynamic signatures from WDK declarations.
static __typeof__(&KeRaiseIrqlToDpcLevel) Raise;
static __typeof__(&KeGetCurrentProcessorNumberEx) Processor;
static DEVICE Device;
static void *Work;
static U64 DpcStorage[8];
static volatile U32 WorkerDone, DpcDone, WorkerFailed, DpcFailed;

static NTSTATUS probe(void) {
  const U8 Previous = KeGetCurrentIrql();
  CHECK(Previous <= 2);
  CHECK(!KeGetCurrentProcessorNumberEx(0));
  CHECK(!Processor(0));
  for (U32 I = 0; I < 2; ++I) {
    U8 Bytes[16];
    for (U32 J = 0; J < 16; ++J)
      Bytes[J] = 0xa5;
    const U32 Index = I ? Processor((NUMBER *)(Bytes + 4))
                        : KeGetCurrentProcessorNumberEx((NUMBER *)(Bytes + 4));
    CHECK(Index == 0 && KeQueryActiveProcessors() == 1);
    for (U32 J = 0; J < 16; ++J)
      CHECK(Bytes[J] == (J >= 4 && J < 8 ? 0 : 0xa5));
  }
  CHECK(KeRaiseIrqlToDpcLevel() == Previous);
  CHECK(KeGetCurrentIrql() == 2);
  CHECK(Raise() == 2);
  KeLowerIrql(2);
  KeLowerIrql(Previous);
  CHECK(KeGetCurrentIrql() == Previous);
  U8 Saved;
#ifdef NEVERD_WDK_FIXTURE
  KeRaiseIrql(HIGH_LEVEL, &Saved);
#else
  Saved = KfRaiseIrql(15);
#endif
  NUMBER Number;
  Number.Group = 0xffff;
  Number.Number = Number.Reserved = 0xff;
  CHECK(!Processor(&Number));
  CHECK(!Number.Group && !Number.Number && !Number.Reserved);
  CHECK(KeGetCurrentIrql() == 15);
  KeLowerIrql(Saved);
  return 0;
}

static void worker(DEVICE Owner, void *Context) {
  WorkerFailed = Owner != Device || Context != Device ||
                 KeGetCurrentIrql() != 0 || probe();
  IoFreeWorkItem(Work);
  WorkerDone = 1;
}
static void dpc(DPC Object, void *Context, void *First, void *Second) {
  DpcFailed = Object != (void *)DpcStorage || Context != Device || First ||
              Second || KeGetCurrentIrql() != 2 || probe();
  DpcDone = 1;
}
static void unload(DRIVER Driver) {
  (void)Driver;
  if (WorkerDone && DpcDone && !WorkerFailed && !DpcFailed)
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;

NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  ROUTINE_NAME Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"KeRaiseIrqlToDpcLevel");
  Raise = (__typeof__(Raise))MmGetSystemRoutineAddress(&Name);
  RtlInitUnicodeString(&Name, (const U16 *)L"KeGetCurrentProcessorNumberEx");
  Processor = (__typeof__(Processor))MmGetSystemRoutineAddress(&Name);
  CHECK(Raise && Processor);
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  if (Registry->Length && Registry->Buffer[Registry->Length / 2 - 1] == 'H') {
#ifdef NEVERD_WDK_FIXTURE
    U8 Previous;
    KeRaiseIrql(3, &Previous);
#else
    KfRaiseIrql(3);
#endif
    Raise();
    return (NTSTATUS)0xc006ffff;
  }
  const NTSTATUS Status = probe();
  CHECK(!Status);
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  Work = IoAllocateWorkItem(Device);
  CHECK(Work);
  KeInitializeDpc((void *)DpcStorage, dpc, Device);
  CHECK(KeInsertQueueDpc((void *)DpcStorage, 0, 0));
  IoQueueWorkItem(Work, worker, 1, Device);
  return 0;
}

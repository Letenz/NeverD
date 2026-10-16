//===- driver_int64_strings.c - Original full-width integer fixture -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify imported and dynamically resolved integer formatting with WDK ABI.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ntddk.h>
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
typedef UNICODE_STRING TEXT;
_Static_assert(sizeof(TEXT) == 16, "Unicode record width");
_Static_assert(FIELD_OFFSET(TEXT, Buffer) == 8, "Unicode buffer offset");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} TEXT, *REGISTRY;
__declspec(dllimport) NTSTATUS RtlInt64ToUnicodeString(U64, U32, TEXT *);
__declspec(dllimport) NTSTATUS RtlIntegerToUnicodeString(U32, U32, TEXT *);
__declspec(dllimport) void RtlInitUnicodeString(TEXT *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(TEXT *);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) U8 KeGetCurrentIrql(void);
__declspec(dllimport) NTSTATUS IoCreateDevice(DRIVER, U32, void *, U32, U32, U8,
                                              DEVICE *);
__declspec(dllimport) void IoDeleteDevice(DEVICE);
__declspec(dllimport) void *IoAllocateWorkItem(DEVICE);
__declspec(dllimport) void IoQueueWorkItem(void *, void (*)(DEVICE, void *),
                                           U32, void *);
__declspec(dllimport) void IoFreeWorkItem(void *);
#endif

#define CHECK(C)                                                               \
  do {                                                                         \
    if (!(C))                                                                  \
      return (NTSTATUS)(0xc0070000u + __LINE__);                               \
  } while (0)

static __typeof__(&RtlInt64ToUnicodeString) Dynamic;
static DEVICE Device;
static void *Work;
static volatile U32 WorkerDone, WorkerFailed;
static const struct {
  U64 Value;
  U32 Base;
  const char *Expected;
} Cases[] = {
    {0, 0, "0"},
    {0x100000000ULL, 10, "4294967296"},
    {~0ULL, 0, "18446744073709551615"},
    {~0ULL, 10, "18446744073709551615"},
    {~0ULL, 16, "FFFFFFFFFFFFFFFF"},
    {~0ULL, 8, "1777777777777777777777"},
    {~0ULL, 2,
     "1111111111111111111111111111111111111111111111111111111111111111"},
    {0x123456789abcdef0ULL, 16, "123456789ABCDEF0"},
    {0x8000000000000000ULL, 10, "9223372036854775808"},
};

static NTSTATUS probe(void) {
  CHECK(KeGetCurrentIrql() == 0);
  for (U32 Which = 0; Which < 2; ++Which) {
    for (U32 I = 0; I < sizeof(Cases) / sizeof(Cases[0]); ++I) {
      U16 Buffer[68];
      for (U32 J = 0; J < 68; ++J)
        Buffer[J] = 0xa5a5;
      TEXT Text = {0x7777, 132, Buffer + 1};
      NTSTATUS Status =
          Which ? Dynamic(Cases[I].Value, Cases[I].Base, &Text)
                : RtlInt64ToUnicodeString(Cases[I].Value, Cases[I].Base, &Text);
      CHECK(!Status && Text.MaximumLength == 132 && Text.Buffer == Buffer + 1);
      U32 Length = 0;
      while (Cases[I].Expected[Length]) {
        CHECK(Buffer[Length + 1] == (U8)Cases[I].Expected[Length]);
        ++Length;
      }
      CHECK(Text.Length == Length * 2 && Buffer[Length + 1] == 0);
      CHECK(Buffer[0] == 0xa5a5);
      for (U32 J = Length + 2; J < 68; ++J)
        CHECK(Buffer[J] == 0xa5a5);
    }
    const U16 Capacities[] = {0, 1, 127, 128, 129, 130, 131};
    for (U32 I = 0; I < sizeof(Capacities) / sizeof(Capacities[0]); ++I) {
      U16 Buffer[68];
      for (U32 J = 0; J < 68; ++J)
        Buffer[J] = 0xa5a5;
      TEXT Text = {0x7777, Capacities[I], Buffer + 1};
      NTSTATUS Status = Which ? Dynamic(~0ULL, 2, &Text)
                              : RtlInt64ToUnicodeString(~0ULL, 2, &Text);
      CHECK((U32)Status == (Capacities[I] < 130 ? 0x80000005u : 0));
      CHECK(Text.Length == 128 && Text.MaximumLength == Capacities[I]);
      CHECK(Text.Buffer == Buffer + 1);
      for (U32 J = 0; J < 68; ++J) {
        U16 Expected = Capacities[I] < 130 || !J || J >= 66 ? 0xa5a5
                       : J == 65                            ? 0
                                                            : '1';
        CHECK(Buffer[J] == Expected);
      }
    }
    CHECK((U32)(Which ? Dynamic(1, 3, 0) : RtlInt64ToUnicodeString(1, 3, 0)) ==
          0xc000000du);
  }
  U16 Buffer[4];
  TEXT Narrow = {0, sizeof(Buffer), Buffer};
  CHECK(!RtlIntegerToUnicodeString((U32)0x100000007ULL, 10, &Narrow));
  CHECK(Narrow.Length == 2 && Buffer[0] == '7' && !Buffer[1]);
  return 0;
}

static void worker(DEVICE Owner, void *Context) {
  WorkerFailed = Owner != Device || Context != Device || probe();
  IoFreeWorkItem(Work);
  WorkerDone = 1;
}
static void unload(DRIVER Driver) {
  (void)Driver;
  if (WorkerDone && !WorkerFailed && !probe())
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;

NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  TEXT Name;
  RtlInitUnicodeString(&Name, (const U16 *)L"RtlInt64ToUnicodeString");
  Dynamic = (__typeof__(Dynamic))MmGetSystemRoutineAddress(&Name);
  CHECK(Dynamic);
  if (Registry->Length && Registry->Buffer[Registry->Length / 2 - 1] == 'H') {
#ifdef NEVERD_WDK_FIXTURE
    U8 Previous;
    KeRaiseIrql(APC_LEVEL, &Previous);
#else
    KfRaiseIrql(1);
#endif
    Dynamic(1, 3, 0);
    return (NTSTATUS)0xc007ffff;
  }
  CHECK(!probe());
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  Work = IoAllocateWorkItem(Device);
  CHECK(Work);
  IoQueueWorkItem(Work, worker, 1, Device);
  return 0;
}

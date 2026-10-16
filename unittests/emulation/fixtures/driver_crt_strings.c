//===- driver_crt_strings.c - Checked kernel CRT calls --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise imported and dynamically resolved CRT calls with original and WDK
/// declarations, including worker/unload lifetimes and dispatch-level buffers.

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned long U32;
typedef unsigned long long U64;
#ifdef NEVERD_WDK_FIXTURE
#include <ctype.h>
#include <ntddk.h>
#include <ntimage.h>
#include <string.h>
// The pinned WDK lacks this declaration; use its PE type for the kernel ABI.
NTSYSAPI PIMAGE_NT_HEADERS NTAPI RtlImageNtHeader(PVOID Base);
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
typedef UNICODE_STRING TEXT;
_Static_assert(sizeof(wchar_t) == 2 && sizeof(size_t) == 8, "x64 CRT widths");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} TEXT, *REGISTRY;
__declspec(dllimport) int memcpy_s(void *, U64, const void *, U64);
__declspec(dllimport) int memmove_s(void *, U64, const void *, U64);
__declspec(dllimport) void *RtlImageNtHeader(void *);
__declspec(dllimport) int isdigit(int);
__declspec(dllimport) char *_strupr(char *);
__declspec(dllimport) U16 *_wcslwr(U16 *);
__declspec(dllimport) U16 *_wcsupr(U16 *);
__declspec(dllimport) int strcpy_s(char *, U64, const char *);
__declspec(dllimport) int strncpy_s(char *, U64, const char *, U64);
__declspec(dllimport) int wcscat_s(U16 *, U64, const U16 *);
__declspec(dllimport) int wcsncpy_s(U16 *, U64, const U16 *, U64);
__declspec(dllimport) void RtlInitUnicodeString(TEXT *, const U16 *);
__declspec(dllimport) void *MmGetSystemRoutineAddress(TEXT *);
__declspec(dllimport) U8 KfRaiseIrql(U8);
__declspec(dllimport) void KeLowerIrql(U8);
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
      return (NTSTATUS)(0xc0080000u + __LINE__);                               \
  } while (0)

// Parenthesized function names bypass the WDK isdigit macro. Volatile
// pointers retain real kernel calls instead of compiler builtin folding.
static __typeof__(&(isdigit)) volatile Digit = (isdigit);
static __typeof__(&_strupr) volatile NarrowUpper = _strupr;
static __typeof__(&_wcslwr) volatile WideLower = _wcslwr;
static __typeof__(&_wcsupr) volatile WideUpper = _wcsupr;
static __typeof__(&strcpy_s) volatile Copy = strcpy_s;
static __typeof__(&strncpy_s) volatile CountedCopy = strncpy_s;
static __typeof__(&wcscat_s) volatile Append = wcscat_s;
static __typeof__(&wcsncpy_s) volatile WideCopy = wcsncpy_s;
static __typeof__(&(isdigit)) DynamicDigit;
static __typeof__(&_strupr) DynamicNarrowUpper;
static __typeof__(&_wcslwr) DynamicWideLower;
static __typeof__(&_wcsupr) DynamicWideUpper;
static __typeof__(&strcpy_s) DynamicCopy;
static __typeof__(&strncpy_s) DynamicCountedCopy;
static __typeof__(&wcscat_s) DynamicAppend;
static __typeof__(&wcsncpy_s) DynamicWideCopy;
// The WDK links memcpy_s from ntoskrnl.lib; dynamic lookup still calls the
// kernel export. The original declarations exercise its direct import.
static __typeof__(&memcpy_s) volatile CheckedMemoryCopy = memcpy_s;
static __typeof__(&memmove_s) volatile CheckedMemoryMove = memmove_s;
static __typeof__(&RtlImageNtHeader) volatile ImageHeader = RtlImageNtHeader;
static __typeof__(&memcpy_s) DynamicMemoryCopy;
static __typeof__(&memmove_s) DynamicMemoryMove;
static __typeof__(&RtlImageNtHeader) DynamicImageHeader;
static DEVICE Device;
static void *Work;
static volatile U32 WorkerDone, WorkerFailed;

static NTSTATUS probe(void) {
  for (U32 Dynamic = 0; Dynamic < 2; ++Dynamic) {
    int (*Classify)(int) = Dynamic ? DynamicDigit : Digit;
    CHECK(Classify('0') == 4 && Classify('9') == 4 && !Classify('/') &&
          !Classify(':') && Classify(0x130) == 4 && !Classify(-1));
    char MemoryBytes[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    CHECK(!(Dynamic ? DynamicMemoryCopy : CheckedMemoryCopy)(MemoryBytes + 2, 4,
                                                             MemoryBytes, 4));
    CHECK(MemoryBytes[0] == 'A' && MemoryBytes[1] == 'B' &&
          MemoryBytes[2] == 'A' && MemoryBytes[3] == 'B' &&
          MemoryBytes[4] == 'C' && MemoryBytes[5] == 'D' &&
          MemoryBytes[6] == 'G' && MemoryBytes[7] == 'H');
    CHECK((Dynamic ? DynamicMemoryMove : CheckedMemoryMove)(MemoryBytes, 2, 0,
                                                            3) == 22);
    CHECK(MemoryBytes[0] == 'A' && MemoryBytes[1] == 'B');
    CHECK((Dynamic ? DynamicMemoryCopy : CheckedMemoryCopy)(MemoryBytes, 2, 0,
                                                            3) == 22);
    CHECK(!MemoryBytes[0] && !MemoryBytes[1] && MemoryBytes[2] == 'A');
    CHECK(!(Dynamic ? DynamicMemoryMove : CheckedMemoryMove)(
        MemoryBytes, ~0ULL, MemoryBytes + 2, 3));
    CHECK(MemoryBytes[0] == 'A' && MemoryBytes[1] == 'B' &&
          MemoryBytes[2] == 'C');
    CHECK(!(Dynamic ? DynamicMemoryCopy : CheckedMemoryCopy)(0, ~0ULL, 0, 0));
    CHECK(!(Dynamic ? DynamicMemoryMove : CheckedMemoryMove)(0, ~0ULL, 0, 0));
    U32 Header[64];
    for (U32 I = 0; I < 64; ++I)
      Header[I] = 0xffffffffu;
    Header[0] = 0x5a4d;
    Header[15] = 0x80;
    Header[32] = 0x4550;
    CHECK((void *)(Dynamic ? DynamicImageHeader : ImageHeader)(Header) ==
          (void *)&Header[32]);
    Header[0] = 0;
    CHECK(!(Dynamic ? DynamicImageHeader : ImageHeader)(Header));
    char Narrow[] = {'a', 'Z', (char)0xe9, 0};
    U16 Wide[] = {'A', 'z', 0x00c4, 0xff41, 0xd800, 0};
    CHECK((Dynamic ? DynamicNarrowUpper : NarrowUpper)(Narrow) == Narrow);
    CHECK(Narrow[0] == 'A' && Narrow[1] == 'Z' && (U8)Narrow[2] == 0xe9);
    CHECK((Dynamic ? DynamicWideLower : WideLower)(Wide) == Wide);
    CHECK(Wide[0] == 'a' && Wide[1] == 'z' && Wide[2] == 0x00c4);
    CHECK((Dynamic ? DynamicWideUpper : WideUpper)(Wide) == Wide);
    CHECK(Wide[0] == 'A' && Wide[1] == 'Z' && Wide[2] == 0x00c4 &&
          Wide[3] == 0xff41 && Wide[4] == 0xd800 && !Wide[5]);
    char Bytes[8];
    U16 Units[8];
    for (U32 I = 0; I < 8; ++I) {
      Bytes[I] = '!';
      Units[I] = '!';
    }
    CHECK((Dynamic ? DynamicCopy : Copy)(Bytes, 3, "ABCD") == 34);
    CHECK(!Bytes[0] && Bytes[1] == 'B' && Bytes[2] == 'C' && Bytes[3] == '!');
    CHECK((Dynamic ? DynamicCountedCopy : CountedCopy)(Bytes, 3, "ABCD",
                                                       ~0ULL) == 80);
    CHECK(Bytes[0] == 'A' && Bytes[1] == 'B' && !Bytes[2] && Bytes[3] == '!');
    CHECK(!(Dynamic ? DynamicCountedCopy : CountedCopy)(Bytes, 8, "ABCD", 2));
    CHECK(Bytes[0] == 'A' && Bytes[1] == 'B' && !Bytes[2] && Bytes[3] == '!');
    CHECK((Dynamic ? DynamicWideCopy : WideCopy)(Units, 3, (const U16 *)L"ABCD",
                                                 ~0ULL) == 80);
    CHECK(Units[0] == 'A' && Units[1] == 'B' && !Units[2] && Units[3] == '!');
    CHECK((Dynamic ? DynamicAppend : Append)(Units, 4, (const U16 *)L"CD") ==
          34);
    CHECK(!Units[0] && Units[1] == 'B' && Units[2] == 'C' && Units[3] == 'D' &&
          Units[4] == '!');
    CHECK(!(Dynamic ? DynamicAppend : Append)(Units, 8, (const U16 *)L"CD"));
    CHECK(Units[0] == 'C' && Units[1] == 'D' && !Units[2] && Units[3] == 'D');
    CHECK(!(Dynamic ? DynamicCountedCopy : CountedCopy)(0, 0, 0, 0));
    CHECK(!(Dynamic ? DynamicWideCopy : WideCopy)(0, 0, 0, 0));
  }
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
#define RESOLVE(Variable, Text)                                                \
  RtlInitUnicodeString(&Name, (const U16 *)L##Text);                           \
  Variable = (__typeof__(Variable))MmGetSystemRoutineAddress(&Name);           \
  CHECK(Variable)
  RESOLVE(DynamicMemoryCopy, "memcpy_s");
  RESOLVE(DynamicMemoryMove, "memmove_s");
  RESOLVE(DynamicImageHeader, "RtlImageNtHeader");
  RESOLVE(DynamicDigit, "isdigit");
  RESOLVE(DynamicNarrowUpper, "_strupr");
  RESOLVE(DynamicWideLower, "_wcslwr");
  RESOLVE(DynamicWideUpper, "_wcsupr");
  RESOLVE(DynamicCopy, "strcpy_s");
  RESOLVE(DynamicCountedCopy, "strncpy_s");
  RESOLVE(DynamicAppend, "wcscat_s");
  RESOLVE(DynamicWideCopy, "wcsncpy_s");
#undef RESOLVE
  U8 Old = 0;
  const U8 Elevated =
      Registry->Length && Registry->Buffer[Registry->Length / 2 - 1] == 'H';
  if (Elevated) {
#ifdef NEVERD_WDK_FIXTURE
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
#else
    Old = KfRaiseIrql(2);
#endif
  }
  CHECK(!probe());
  if (Elevated)
    KeLowerIrql(Old);
  CHECK(!IoCreateDevice(Driver, 0, 0, 0x22, 0, 0, &Device));
  *(void (**)(DRIVER))((U8 *)Driver + 0x68) = Unload;
  Work = IoAllocateWorkItem(Device);
  CHECK(Work);
  IoQueueWorkItem(Work, worker, 1, Device);
  return 0;
}

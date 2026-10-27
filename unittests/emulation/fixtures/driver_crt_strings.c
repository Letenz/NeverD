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
#include <stdarg.h>
#ifdef NEVERD_WDK_FIXTURE
#include <ctype.h>
#include <ntifs.h>
#include <ntimage.h>
#include <string.h>
// The pinned WDK lacks this declaration; use its PE type for the kernel ABI.
NTSYSAPI PIMAGE_NT_HEADERS NTAPI RtlImageNtHeader(PVOID Base);
// These exported query ABIs are absent from the pinned WDK declarations.
NTSYSAPI PVOID NTAPI RtlPcToFileHeader(PVOID PC, PVOID *ImageBase);
NTKERNELAPI KPROCESSOR_MODE NTAPI PsGetCurrentThreadPreviousMode(VOID);
typedef PDEVICE_OBJECT DEVICE;
typedef PDRIVER_OBJECT DRIVER;
typedef PUNICODE_STRING REGISTRY;
typedef UNICODE_STRING TEXT;
_Static_assert(sizeof(wchar_t) == 2 && sizeof(wint_t) == 2 &&
                   sizeof(size_t) == 8,
               "x64 CRT widths");
#else
typedef int NTSTATUS;
typedef void *DEVICE;
typedef void *DRIVER;
typedef struct {
  U16 Length, MaximumLength;
  U16 *Buffer;
} TEXT, *REGISTRY;
typedef union {
  long long QuadPart;
} LARGE_INTEGER;
__declspec(dllimport) NTSTATUS RtlHashUnicodeString(const TEXT *, U8, U32,
                                                    U32 *);
__declspec(dllimport) NTSTATUS RtlUpcaseUnicodeString(TEXT *, const TEXT *, U8);
__declspec(dllimport) U8 RtlTimeToSecondsSince1980(LARGE_INTEGER *, U32 *);
__declspec(dllimport) void RtlFreeUnicodeString(TEXT *);
__declspec(dllimport) int toupper(int);
__declspec(dllimport) U16 towupper(U16);
__declspec(dllimport) U16 RtlUpcaseUnicodeChar(U16);
__declspec(dllimport) U32 vDbgPrintExWithPrefix(const char *, U32, U32,
                                                const char *, va_list);
__declspec(dllimport) int memcpy_s(void *, U64, const void *, U64);
__declspec(dllimport) int memmove_s(void *, U64, const void *, U64);
__declspec(dllimport) void *RtlImageNtHeader(void *);
__declspec(dllimport) void *RtlPcToFileHeader(void *, void **);
__declspec(dllimport) char PsGetCurrentThreadPreviousMode(void);
__declspec(dllimport) char ExGetPreviousMode(void);
__declspec(dllimport) void *IoThreadToProcess(void *);
__declspec(dllimport) void *PsGetCurrentThread(void);
__declspec(dllimport) void *PsGetCurrentProcess(void);
__declspec(dllimport) U16 ObGetFilterVersion(void);
__declspec(dllimport) void *IoGetInitialStack(void);
__declspec(dllimport) void IoGetStackLimits(U64 *, U64 *);
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
static __typeof__(&RtlHashUnicodeString) volatile Hash = RtlHashUnicodeString;
static __typeof__(&RtlUpcaseUnicodeString) volatile Upcase =
    RtlUpcaseUnicodeString;
static __typeof__(&RtlTimeToSecondsSince1980) volatile Seconds =
    RtlTimeToSecondsSince1980;
static __typeof__(&RtlHashUnicodeString) DynamicHash;
static __typeof__(&RtlUpcaseUnicodeString) DynamicUpcase;
static __typeof__(&RtlTimeToSecondsSince1980) DynamicSeconds;
static __typeof__(&(toupper)) volatile ByteUpper = (toupper);
static __typeof__(&(towupper)) volatile UnitUpper = (towupper);
static __typeof__(&RtlUpcaseUnicodeChar) volatile UnicodeUpper =
    RtlUpcaseUnicodeChar;
static __typeof__(&(toupper)) DynamicByteUpper;
static __typeof__(&(towupper)) DynamicUnitUpper;
static __typeof__(&RtlUpcaseUnicodeChar) DynamicUnicodeUpper;
static __typeof__(&vDbgPrintExWithPrefix) volatile Print =
    vDbgPrintExWithPrefix;
static __typeof__(&vDbgPrintExWithPrefix) DynamicPrint;
static __typeof__(&IoThreadToProcess) volatile ThreadProcess =
    IoThreadToProcess;
static __typeof__(&PsGetCurrentThreadPreviousMode) volatile PreviousMode =
    PsGetCurrentThreadPreviousMode;
static __typeof__(&RtlPcToFileHeader) volatile PcImage = RtlPcToFileHeader;
static __typeof__(&IoThreadToProcess) DynamicThreadProcess;
static __typeof__(&PsGetCurrentThreadPreviousMode) DynamicPreviousMode;
static __typeof__(&RtlPcToFileHeader) DynamicPcImage;
static __typeof__(&ObGetFilterVersion) volatile FilterVersion =
    ObGetFilterVersion;
static __typeof__(&ObGetFilterVersion) DynamicFilterVersion;
static __typeof__(&IoGetInitialStack) volatile InitialStack = IoGetInitialStack;
static __typeof__(&IoGetStackLimits) volatile StackLimits = IoGetStackLimits;
static __typeof__(&IoGetInitialStack) DynamicInitialStack;
static __typeof__(&IoGetStackLimits) DynamicStackLimits;
static U64 EntryStackTop;
static void *ImageBase;
static DEVICE Device;
static void *Work;
static volatile U32 WorkerDone, WorkerFailed;

static U32 print(U32 Dynamic, const char *Prefix, const char *Format, ...) {
  va_list Arguments;
  va_start(Arguments, Format);
  U32 Status =
      (Dynamic ? DynamicPrint : Print)(Prefix, 7, 2, Format, Arguments);
  va_end(Arguments);
  return Status;
}

// Counted helpers permit APC_LEVEL. The entry probes at that level; worker
// and unload probe at PASSIVE_LEVEL, separately from the CRT's DISPATCH run.
static NTSTATUS countedProbe(void) {
  for (U32 Dynamic = 0; Dynamic < 2; ++Dynamic) {
    CHECK((Dynamic ? DynamicFilterVersion : FilterVersion)() == 0x100);
    CHECK((Dynamic ? DynamicByteUpper : ByteUpper)('a') == 'A');
    CHECK((Dynamic ? DynamicByteUpper : ByteUpper)(0x15a) == 'Z');
    CHECK((Dynamic ? DynamicUnitUpper : UnitUpper)('z') == 'Z');
    CHECK((Dynamic ? DynamicUnitUpper : UnitUpper)(0x00bf) == 0x00bf);
    CHECK((Dynamic ? DynamicUnicodeUpper : UnicodeUpper)('b') == 'B');
    CHECK((Dynamic ? DynamicUnicodeUpper : UnicodeUpper)(0x00bf) == 0x00bf);
    U16 Input[] = {'a', 'b', 0x00bf};
    U16 Output[] = {'!', '!', '!', '!'};
    TEXT Source = {5, 0, Input};
    TEXT Destination = {0, sizeof(Output), Output};
    U32 Value = 0xffffffffu;
    CHECK(!(Dynamic ? DynamicHash : Hash)(&Source, 1, 0, &Value));
    CHECK(Value == 4264001);
    CHECK(!(Dynamic ? DynamicUpcase : Upcase)(&Destination, &Source, 0));
    CHECK(Destination.Length == 5 && Destination.MaximumLength == 8 &&
          Output[0] == 'A' && Output[1] == 'B' && Output[2] == '!');
    Destination.MaximumLength = 4;
    Destination.Length = 0x1234;
    CHECK((U32)(Dynamic ? DynamicUpcase : Upcase)(&Destination, &Source, 0) ==
          0x80000005u);
    CHECK(Destination.Length == 0x1234 && Output[2] == '!');
    CHECK(!(Dynamic ? DynamicUpcase : Upcase)(&Destination, &Source, 1));
    CHECK(Destination.Length == 5 && Destination.MaximumLength == 5 &&
          Destination.Buffer[0] == 'A' && Destination.Buffer[1] == 'B');
    RtlFreeUnicodeString(&Destination);
    CHECK(!Destination.Buffer && !Destination.Length &&
          !Destination.MaximumLength);
    Source.Length = 6;
    CHECK(!(Dynamic ? DynamicHash : Hash)(&Source, 1, 1, &Value));
    CHECK(Value == 541327550u);
    CHECK((U32)(Dynamic ? DynamicHash : Hash)(&Source, 0, 2, &Value) ==
          0xc000000du);
    CHECK(!Value);
    LARGE_INTEGER Time;
    Time.QuadPart = 119600064000000000LL + 1239999999;
    CHECK((Dynamic ? DynamicSeconds : Seconds)(&Time, &Value));
    CHECK(Value == 123);
    Time.QuadPart = 119600064000000000LL - 1;
    CHECK(!(Dynamic ? DynamicSeconds : Seconds)(&Time, &Value));
    CHECK(Value == 123);
    Time.QuadPart = 119600064000000000LL + 0x100000000LL * 10000000 - 1;
    CHECK((Dynamic ? DynamicSeconds : Seconds)(&Time, &Value));
    CHECK(Value == 0xffffffffu);
    Time.QuadPart++;
    CHECK(!(Dynamic ? DynamicSeconds : Seconds)(&Time, &Value));
    CHECK(Value == 0xffffffffu);
  }
  return 0;
}

static NTSTATUS probe(void) {
  for (U32 Dynamic = 0; Dynamic < 2; ++Dynamic) {
    CHECK((Dynamic ? DynamicThreadProcess : ThreadProcess)(
              PsGetCurrentThread()) == PsGetCurrentProcess());
    char Mode = (Dynamic ? DynamicPreviousMode : PreviousMode)();
    CHECK(!Mode && Mode == ExGetPreviousMode());
    void *Found = (void *)~0ULL;
    CHECK((Dynamic ? DynamicPcImage : PcImage)((void *)probe, &Found) ==
          ImageBase);
    CHECK(Found == ImageBase);
    CHECK(!(Dynamic ? DynamicPcImage : PcImage)((void *)1, &Found));
    CHECK(!Found);
    CHECK(!print(Dynamic, "crt:", "%hhd|%I64x|%.*s", 384, 0x1122334455667788ULL,
                 3, "abcd"));
    // Keep the long prefix in image data so formatting, not a fill loop,
    // consumes the native corpus instruction budget.
    static const char Prefix[] =
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"
        "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP";
    CHECK(!print(Dynamic, Prefix, ""));
    CHECK(!print(Dynamic, "crt:", "%0512u", 0));
    CHECK(!print(Dynamic, "crt:", "A%cz", 0));
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

static NTSTATUS stackProbe(U32 Phase) {
  for (U32 Dynamic = 0; Dynamic < 2; ++Dynamic) {
    U64 Low = 0, High = 0;
    (Dynamic ? DynamicStackLimits : StackLimits)(&Low, &High);
    const U64 Initial = (U64)(Dynamic ? DynamicInitialStack : InitialStack)();
    CHECK(Low <= (U64)&Low && (U64)&Low < High);
    CHECK(Initial > (U64)&Low && Initial <= High);
    if (!Phase) {
      if (EntryStackTop)
        CHECK(High == EntryStackTop);
      EntryStackTop = High;
    } else
      CHECK(Phase == 1 ? High != EntryStackTop : High == EntryStackTop);
    const U64 SavedLow = Low, SavedHigh = High;
    U8 Old;
#ifdef NEVERD_WDK_FIXTURE
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
#else
    Old = KfRaiseIrql(2);
#endif
    (Dynamic ? DynamicStackLimits : StackLimits)(&Low, &High);
    KeLowerIrql(Old);
    CHECK(Low == SavedLow && High == SavedHigh);
  }
  return 0;
}

static void worker(DEVICE Owner, void *Context) {
  WorkerFailed = Owner != Device || Context != Device || probe() ||
                 countedProbe() || stackProbe(1);
  IoFreeWorkItem(Work);
  WorkerDone = 1;
}
static void unload(DRIVER Driver) {
  (void)Driver;
  if (WorkerDone && !WorkerFailed && !probe() && !countedProbe() &&
      !stackProbe(2))
    IoDeleteDevice(Device);
}
static void (*volatile Unload)(DRIVER) = unload;

NTSTATUS DriverEntry(DRIVER Driver, REGISTRY Registry) {
  ImageBase = *(void **)((U8 *)Driver + 0x18);
  TEXT Name;
#define RESOLVE(Variable, Text)                                                \
  RtlInitUnicodeString(&Name, (const U16 *)L##Text);                           \
  Variable = (__typeof__(Variable))MmGetSystemRoutineAddress(&Name);           \
  CHECK(Variable)
  RESOLVE(DynamicMemoryCopy, "memcpy_s");
  RESOLVE(DynamicMemoryMove, "memmove_s");
  RESOLVE(DynamicImageHeader, "RtlImageNtHeader");
  RESOLVE(DynamicHash, "RtlHashUnicodeString");
  RESOLVE(DynamicUpcase, "RtlUpcaseUnicodeString");
  RESOLVE(DynamicSeconds, "RtlTimeToSecondsSince1980");
  RESOLVE(DynamicByteUpper, "toupper");
  RESOLVE(DynamicUnitUpper, "towupper");
  RESOLVE(DynamicUnicodeUpper, "RtlUpcaseUnicodeChar");
  RESOLVE(DynamicPrint, "vDbgPrintExWithPrefix");
  RESOLVE(DynamicThreadProcess, "IoThreadToProcess");
  RESOLVE(DynamicPreviousMode, "PsGetCurrentThreadPreviousMode");
  RESOLVE(DynamicPcImage, "RtlPcToFileHeader");
  RESOLVE(DynamicFilterVersion, "ObGetFilterVersion");
  RESOLVE(DynamicInitialStack, "IoGetInitialStack");
  RESOLVE(DynamicStackLimits, "IoGetStackLimits");
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
#ifdef NEVERD_WDK_FIXTURE
  KeRaiseIrql(APC_LEVEL, &Old);
#else
  Old = KfRaiseIrql(1);
#endif
  CHECK(!countedProbe());
  CHECK(!stackProbe(0));
  KeLowerIrql(Old);
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

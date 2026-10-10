//===- windows_system.c - Original system DLL lookup observations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef unsigned char U8;
#define NEVERD_SYSTEM_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_SYSTEM_MODE(Name, Value) enum { Name = Value };
#define NEVERD_SYSTEM_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_SYSTEM_WIDE(Name, Text) static const U16 Name[] = Text;
#include "WindowsSystemCases.def"
#undef NEVERD_SYSTEM_WIDE
#undef NEVERD_SYSTEM_TEXT
#undef NEVERD_SYSTEM_MODE
#undef NEVERD_SYSTEM_VALUE
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) U32 GetCurrentProcessId(void);
__declspec(dllimport) void *GetModuleHandleW(const U16 *);
__declspec(dllimport) void *GetModuleHandleA(const char *);
__declspec(dllimport) void *LoadLibraryW(const U16 *);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) U64 VirtualQuery(const void *, void *, U64);
__declspec(dllimport) int VirtualProtect(void *, U64, U32, U32 *);
typedef struct {
  void *Base, *AllocationBase;
  U32 AllocationProtect, Padding;
  U64 Size;
  U32 State, Protect, Type, Tail;
} MemoryInformation;
static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}
static U32 word(const void *P, U32 Offset) {
  return *(const U32 *)((const U8 *)P + Offset);
}
static int equal(const char *A, const char *B) {
  while (*A && *A == *B) {
    ++A;
    ++B;
  }
  return *A == *B;
}
static void *lookup(void *Module, const char *Name) {
  SetLastError(LastErrorSeed);
  void *Address = GetProcAddress(Module, Name);
  require(Address != 0 && GetLastError() == LastErrorSeed, 1);
  return Address;
}
static void image(void *Module, const char *Symbol) {
  MemoryInformation Info;
  require(VirtualQuery(Module, &Info, sizeof(Info)) == sizeof(Info), 2);
  require(Info.AllocationBase == Module && Info.State == MemCommit &&
              Info.Type == MemImage,
          3);
  const U8 *Base = Module;
  require(*(const U16 *)Base == DOSMagic, 4);
  const U8 *PE = Base + word(Base, PEHeaderPointer);
  require(word(PE, 0) == PEMagic, 5);
#if defined(_M_X64) || defined(__x86_64__)
  require(*(const U16 *)(PE + PEMachine) == X64Machine, 6);
#else
  require(*(const U16 *)(PE + PEMachine) == ARM64Machine, 6);
#endif
  require(*(const U16 *)(PE + PEOptional) == PE64Magic, 7);
  U32 Size = word(PE, PEImageSize);
  U32 Exports = word(PE, PEExportDirectory);
  U32 ExportSize = word(PE, PEExportDirectory + sizeof(U32));
  require(Exports && Exports < Size && ExportSize <= Size - Exports, 8);
  const U8 *Directory = Base + Exports;
  U32 Count = word(Directory, ExportNameCount);
  require(Count && Count < MaxExports, 9);
  const U32 *Names = (const U32 *)(Base + word(Directory, ExportNames));
  const U16 *Ordinals = (const U16 *)(Base + word(Directory, ExportOrdinals));
  const U32 *Addresses = (const U32 *)(Base + word(Directory, ExportAddresses));
  U32 Found = 0;
  for (U32 I = 0; I < Count; ++I)
    if (equal((const char *)(Base + Names[I]), Symbol)) {
      U32 RVA = Addresses[Ordinals[I]];
      void *API = lookup(Module, Symbol);
      require(RVA && RVA < Size, 10);
      if (RVA < Exports || RVA >= Exports + ExportSize)
        require(API == Base + RVA, 11);
      require(VirtualQuery(API, &Info, sizeof(Info)) == sizeof(Info) &&
                  Info.Type == MemImage && Info.State == MemCommit,
              12);
      ++Found;
    }
  require(Found == 1, 13);
}
static U8 *peb(void) {
  U8 *TEB;
#define NEVERD_SYSTEM_ASM(Name, Text) __asm__(Text : "=r"(TEB));
#include "WindowsSystemCases.def"
#undef NEVERD_SYSTEM_ASM
  return *(U8 **)(TEB + TebPEB);
}
static void version(void *Native, U32 Mode) {
  U8 *PEB = peb();
  U32 Values[] = {word(PEB, PebVersionMajor), word(PEB, PebVersionMinor),
                  *(const U16 *)(PEB + PebVersionBuild),
                  word(PEB, PebVersionPlatform)};
  if (Mode == VersionBytesMode) {
    U32 Written;
    require(WriteFile(GetStdHandle(StdoutSelector), Values, sizeof(Values),
                      &Written, 0) &&
                Written == sizeof(Values),
            201);
    ExitProcess(ExitStatus);
  }
  // Independently compare the internal layout against the native API; the
  // model-only version-byte case does not infer a Windows release from it.
  U32 Information[71];
  for (U32 I = 0; I < 71; ++I)
    ((volatile U32 *)Information)[I] = 0;
  Information[0] = sizeof(Information);
  U32 (*Query)(void *) = lookup(Native, "RtlGetVersion");
  require(Query(Information) == 0, 202);
  for (U32 I = 0; I < 4; ++I)
    require(Values[I] == Information[I + 1], 203 + I);
}
static void lists(void **Modules) {
  U8 *PEB = peb();
  U8 *Ldr = *(U8 **)(PEB + PebLdr);
  const U32 Heads[] = {LdrLoadList, LdrMemoryList, LdrInitList};
  for (U32 I = 0; I < sizeof(Heads) / sizeof(Heads[0]); ++I) {
    U8 *Head = Ldr + Heads[I], *Previous = Head;
    U32 Seen = 0, Count = 0;
    for (U8 *Link = *(U8 **)Head; Link != Head; Link = *(U8 **)Link) {
      require(++Count < MaxModules && ((void **)Link)[1] == Previous, 14);
      U8 *Node = Link - I * sizeof(void *) * 2;
      for (U32 J = 0; J < 3; ++J)
        if (*(void **)(Node + ModuleBase) == Modules[J]) {
          require(!(Seen & (1u << J)) && word(Node, ModuleSize), 15);
          require(*(U16 *)(Node + ModuleName) &&
                      *(void **)(Node + ModuleName + UnicodeBuffer),
                  16);
          Seen |= 1u << J;
        }
      Previous = Link;
    }
    require(((void **)Head)[1] == Previous && Seen == 7, 17);
  }
}
static void loads(void **Modules) {
  const char *Names[] = {Kernel32Name, KernelBaseName, NtdllName};
  const U16 *Wide[] = {Kernel32Wide, KernelBaseWide, NtdllWide};
  for (U32 I = 0; I < RepeatCount; ++I)
    for (U32 J = 0; J < 3; ++J) {
      void *A = LoadLibraryA(Names[J]);
      void *W = LoadLibraryW(Wide[J]);
      require(A == Modules[J] && W == A, 18);
      require(FreeLibrary(A) && FreeLibrary(W), 19);
      require(GetModuleHandleW(Wide[J]) == A, 20);
      require(GetModuleHandleA(Names[J]) == A, 34);
    }
  void *(*Heap)(void) = lookup(Modules[0], HeapName);
  require((void *)Heap == (void *)&GetProcessHeap && Heap() == GetProcessHeap(),
          21);
  Heap = lookup(Modules[1], HeapName);
  require(Heap() == GetProcessHeap(), 22);
  U32 (*PID)(void) = lookup(Modules[0], PIDName);
  require(PID() == GetCurrentProcessId(), 23);
}
static void forward(void) {
  void *Module = LoadLibraryA(ForwardFile);
  require(Module != 0, 24);
  void *(*Heap)(void) = lookup(Module, ForwardName);
  require((void *)Heap == (void *)&GetProcessHeap && Heap() == GetProcessHeap(),
          25);
  require(FreeLibrary(Module), 26);
}
static void clocks(void **Modules) {
  void (*FileTime)(U64 *) = lookup(Modules[0], "GetSystemTimeAsFileTime");
  U32 (*Ticks)(void) = lookup(Modules[0], "GetTickCount");
  int (*Counter)(U64 *) = lookup(Modules[0], "QueryPerformanceCounter");
  int (*Frequency)(U64 *) = lookup(Modules[0], "QueryPerformanceFrequency");
  U32 (*Delay)(U8, const long long *) = lookup(Modules[2], "ZwDelayExecution");
  U64 First, Last, Before, After, Rate;
  FileTime(&First);
  require(Frequency(&Rate) && Rate && Counter(&Before), 36);
  U32 Tick = Ticks();
  const long long Interval = -200000;
  require(Delay(0, &Interval) == 0, 37);
  require(Counter(&After) && After >= Before, 38);
  FileTime(&Last);
  // Independently bound the 1601 epoch to years 2020..2100. A Unix timestamp
  // or a value expressed in nanoseconds cannot satisfy this window.
  require(First >= 132223104000000000ull && First < 157469184000000000ull &&
              Last >= 132223104000000000ull && Last < 157469184000000000ull,
          39);
  require(After - Before >= Rate / 50, 40);
  // Allow wall-clock granularity and an adjustment; do not require UTC to be
  // monotonic. The reported counter frequency must still describe its units.
  if (Last >= First)
    require((After - Before) / Rate <= (Last - First) / 10000000 + 1, 41);
  require((U32)(Ticks() - Tick) < 0x80000000u &&
              GetLastError() == LastErrorSeed,
          42);
}
#include "WindowsCriticalSectionFixture.inc"
#include "WindowsFLSExitFixture.inc"
#include "WindowsNativeServiceFixture.inc"
#include "WindowsSectionFixture.inc"
U32 entry(void) {
  U32 Mode = LoadMode;
  for (const U16 *P = GetCommandLineW(); *P; ++P)
    if (*P == ModePrefix && P[1])
      Mode = P[1];
  SetLastError(LastErrorSeed);
  void *Modules[] = {GetModuleHandleW(Kernel32Wide),
                     GetModuleHandleW(KernelBaseWide),
                     GetModuleHandleW(NtdllWide)};
  require(Modules[0] && Modules[1] && Modules[2] &&
              GetLastError() == LastErrorSeed,
          27);
  require(GetModuleHandleA(0) == GetModuleHandleW(0), 35);
  if (Mode == VersionBytesMode || Mode == VersionNativeMode)
    version(Modules[2], Mode);
  if (Mode == UnknownMode)
    GetProcAddress(Modules[0], UnmodeledName);
  if (Mode == OrdinalMode)
    GetProcAddress(Modules[0], (const char *)(U64)QueryOrdinal);
  if (Mode == ClocksMode)
    clocks(Modules);
  if (Mode == SectionsMode || Mode == SectionNamespaceMode ||
      Mode == SectionAccessMode || Mode == SectionFixedMode ||
      Mode == SectionOffsetMode)
    sections(Modules[2], Mode);
  if (Mode == AbsoluteDelayMode) {
    U32 (*Delay)(U8, const long long *) =
        lookup(Modules[2], "ZwDelayExecution");
    const long long Interval = 0x7fffffffffffffffLL;
    Delay(0, &Interval);
  }
  if (Mode == ChangedMode || Mode == ChangedForwardMode ||
      Mode == ChangedImportMode) {
    void *Module =
        Mode == ChangedForwardMode ? LoadLibraryA(ForwardFile) : Modules[0];
    require(Module != 0, 28);
    U32 Previous;
    require(VirtualProtect(Modules[0], PageSize, PageReadWrite, &Previous), 29);
    *(U16 *)Modules[0] = 0;
    if (Mode == ChangedImportMode)
      LoadLibraryA(ForwardFile);
    GetProcAddress(Module, Mode == ChangedMode ? HeapName : ForwardName);
  }
  if (Mode == LoadMode || Mode == ReturnMode) {
    loads(Modules);
    lists(Modules);
    if (Mode == ReturnMode)
      forward();
  }
  if (Mode == ImagesMode) {
    image(Modules[0], HeapName);
    image(Modules[1], HeapName);
    image(Modules[2], NativeExitName);
    lists(Modules);
  }
  if (Mode == ErrorsMode) {
    require(!GetProcAddress(Modules[0], WrongCaseName) &&
                GetLastError() == ErrorProcedureNotFound,
            30);
    require(!GetProcAddress(Modules[0], EmptyName) &&
                GetLastError() == ErrorProcedureNotFound,
            31);
    require(!GetProcAddress(Modules[0], 0) &&
                GetLastError() == ErrorInvalidParameter,
            32);
  }
  if (Mode == ForwardMode)
    forward();
  if (Mode == NativeServicesMode || Mode == NativeUnknownMode ||
      Mode == NativeUnimplementedMode)
    nativeServices(Modules[2], Mode);
  if (Mode == CriticalSectionsMode || Mode == CriticalUninitializedMode ||
      Mode == CriticalUnownedLeaveMode || Mode == CriticalReinitializeMode ||
      Mode == CriticalDeletedMode || Mode == CriticalCorruptedMode ||
      Mode == CriticalOwnedDeleteMode)
    criticalSections(Modules[0], Modules[2], Mode);
  if (flsMode(Mode))
    flsScenario(Mode);
  U32 Result[] = {1, Mode}, Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Result, sizeof(Result),
                    &Written, 0) &&
              Written == sizeof(Result),
          33);
  if (Mode == ReturnMode || Mode == FLSReturnMode)
    return ExitStatus;
  if (Mode == NativeExitMode || Mode == FLSNativeExitMode) {
    void (*Exit)(U32) = lookup(Modules[2], NativeExitName);
    Exit(ExitStatus);
  }
  ExitProcess(ExitStatus);
  return FailureStatus;
}

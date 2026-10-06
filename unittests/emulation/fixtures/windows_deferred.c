//===- windows_deferred.c - Imports beyond the process model --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
#define NEVERD_DEFERRED_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_DEFERRED_MODE(Name, Value) enum { Name = Value };
#include "WindowsDeferredCases.def"
#undef NEVERD_DEFERRED_MODE
#undef NEVERD_DEFERRED_VALUE
#define NEVERD_DEFERRED_TEXT(Name, Text) static const char Name[] = Text;
#include "WindowsDeferredCases.def"
#undef NEVERD_DEFERRED_TEXT
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) void *GetModuleHandleA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
// Neither of these has a model. Importing them must not prevent loading.
__declspec(dllimport) U32 GetTickCount(void);
__declspec(dllimport) int AbsentRoutine(int);

typedef U32 (*Routine)(void);

static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}

static U32 mode(void) {
  const U16 *P = GetCommandLineW();
  while (*P && *P != ModePrefix)
    ++P;
  return *P ? P[1] : QueryMode;
}

// Resolve everything and execute nothing: addresses of unmodeled exports are
// ordinary values a program may store, compare and pass around.
static void query(void) {
  void *Kernel = GetModuleHandleA(KernelModule);
  void *Unmodeled = (void *)GetTickCount;
  void *Absent = (void *)AbsentRoutine;
  require(Kernel != 0 && Unmodeled != 0 && Absent != 0, 1);
  require(Unmodeled != Absent, 2);
  // One identity has one address, however it is looked up.
  require(GetProcAddress(Kernel, UnmodeledExport) == Unmodeled, 3);
  require(GetProcAddress(Kernel, ModeledExport) == (void *)ExitProcess, 4);
  require((void *)ExitProcess != Unmodeled, 5);
  void *Static = GetModuleHandleA(AbsentModule);
  require(Static != 0 && *(const U16 *)Static == DOSMagic, 6);
  require(GetProcAddress(Static, AbsentExport) == Absent, 7);
  require(GetModuleHandleA(DynamicModule) == 0, 8);
  void *Dynamic = LoadLibraryA(DynamicModule);
  require(Dynamic != 0 && Dynamic != Static && Dynamic != Kernel, 9);
  require(*(const U16 *)Dynamic == DOSMagic, 10);
  require(GetModuleHandleA(DynamicModule) == Dynamic, 11);
  void *First = GetProcAddress(Dynamic, FirstExport);
  void *Second = GetProcAddress(Dynamic, SecondExport);
  void *Numbered = GetProcAddress(Dynamic, (const char *)DynamicOrdinal);
  require(First != 0 && Second != 0 && Numbered != 0, 12);
  require(First != Second && First != Numbered && Second != Numbered, 13);
  require(First != Absent && First != Unmodeled, 14);
  require(GetProcAddress(Dynamic, FirstExport) == First, 15);
  require(FreeLibrary(Dynamic) != 0, 16);
  require(GetModuleHandleA(DynamicModule) == 0, 17);
  // A module loaded again is the same identity with the same entries.
  Dynamic = LoadLibraryA(DynamicModule);
  require(Dynamic != 0, 18);
  require(GetProcAddress(Dynamic, FirstExport) == First, 19);
}

void entry(void) {
  const U32 Mode = mode();
  if (Mode == QueryMode) {
    query();
    ExitProcess(ExitStatus);
  }
  U32 Result = 0;
  if (Mode == StaticMode)
    Result = GetTickCount();
  else if (Mode == ModuleMode)
    Result = (U32)AbsentRoutine(1);
  else {
    void *Dynamic = LoadLibraryA(DynamicModule);
    require(Dynamic != 0, 20);
    Routine Target = (Routine)GetProcAddress(
        Dynamic,
        Mode == OrdinalMode ? (const char *)DynamicOrdinal : FirstExport);
    require(Target != 0, 21);
    Result = Target();
  }
  // Unreachable: an opaque entry stops the run instead of returning.
  ExitProcess(FailureStatus + Result);
}

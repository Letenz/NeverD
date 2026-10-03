//===- windows_exceptions.c - Original vectored exception observations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef unsigned char U8;
#define NEVERD_VEH_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_VEH_MODE(Name, Value) enum { Name = Value };
#define NEVERD_VEH_TEXT(Name, Text) static const char Name[] = Text;
#if defined(_M_X64) || defined(__x86_64__)
#define NEVERD_VEH_CONTEXT_X64(Name, Value) enum { Name = Value };
#else
#define NEVERD_VEH_CONTEXT_ARM64(Name, Value) enum { Name = Value };
#endif
#define NEVERD_VEH_ASM(Name, Text) __asm__(Text);
#include "WindowsExceptionCases.def"
#undef NEVERD_VEH_ASM
#undef NEVERD_VEH_CONTEXT_X64
#undef NEVERD_VEH_CONTEXT_ARM64
#undef NEVERD_VEH_TEXT
#undef NEVERD_VEH_MODE
#undef NEVERD_VEH_VALUE
typedef struct Record {
  U32 Code, Flags;
  struct Record *Nested;
  void *Address;
  U32 Count, Padding;
  U64 Arguments[MaxParameters];
} Record;
typedef struct {
  Record *Record;
  U8 *Context;
} Pointers;
typedef U32 (*Handler)(Pointers *);
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) void *AddVectoredExceptionHandler(U32, Handler);
__declspec(dllimport) U32 RemoveVectoredExceptionHandler(void *);
__declspec(dllimport) void RaiseException(U32, U32, U32, const U64 *);
__declspec(dllimport) void *VirtualAlloc(void *, U64, U32, U32);
__declspec(dllimport) int VirtualProtect(void *, U64, U32, U32 *);
__declspec(dllimport) int VirtualFree(void *, U64, U32);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
extern U64 ReadFault(const void *);
extern void WriteFault(void *, U64);
extern U64 ContextFault(const void *, U64 *);
extern void ContextFaultSite(void), ContextResume(void);
#if defined(_M_X64) || defined(__x86_64__)
extern U64 DivideFault(U64);
extern void DivideFaultSite(void);
#endif
__declspec(align(16))
const U64 VectorSeed[] = {VectorInitialLow, VectorInitialHigh};
static U32 Mode, Calls, Depth, Notifications;
static U64 Trace, GoodData = DataValue;
static void *SelfHandle, *NextHandle, *WritePage;
static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}
static void complete(void) {
  U32 Result[] = {1, Mode}, Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Result, sizeof(Result),
                    &Written, 0) &&
              Written == sizeof(Result),
          1);
  ExitProcess(ExitStatus);
}
static void trace(U32 ID) { Trace = (Trace << 4) | ID; }
static U32 one(Pointers *P) {
  trace(1);
  return ContinueSearch;
}
static U32 two(Pointers *P) {
  trace(2);
  return ContinueSearch;
}
static U32 three(Pointers *P) {
  trace(3);
  return ContinueExecution;
}
static U32 four(Pointers *P) {
  trace(4);
  return ContinueSearch;
}
static U32 victim(Pointers *P) {
  require(0, 2);
  return ContinueSearch;
}
static U32 mutate(Pointers *P) {
  trace(1);
  require(RemoveVectoredExceptionHandler(SelfHandle), 3);
  require(RemoveVectoredExceptionHandler(NextHandle), 4);
  require(AddVectoredExceptionHandler(0, three) != 0, 5);
  require(AddVectoredExceptionHandler(1, two) != 0, 6);
  return ContinueSearch;
}
static U32 resolve(Pointers *P) {
  ++Calls;
  Record *R = P->Record;
  U8 *C = P->Context;
  require(R && C && !((U64)C & 15), 7);
  require((*(U32 *)(C + ContextFlags) & ContextAll) == ContextAll &&
              *(U64 *)(C + ContextPC) && *(U64 *)(C + ContextSP),
          8);
  require(R->Address != 0 && R->Count <= MaxParameters, 9);
  if (R->Code == AttachCode || R->Code == DetachCode) {
    require(Mode == LoaderMode && R->Count == 1 && R->Arguments[0] == DataValue,
            10);
    ++Notifications;
    return ContinueExecution;
  }
  if (Mode == ContextMode && R->Code == ContextProbeCode) {
    require(*(U32 *)(C + ContextControl) == ControlValue, 47);
    return ContinueExecution;
  }
  if (Mode == SoftwareMode) {
    require(R->Code == SoftwareCode && R->Flags == 0 && !R->Nested, 11);
    require(R->Count == (Calls == 1 ? MaxParameters : 0), 12);
    for (U32 I = 0; I < R->Count; ++I)
      require(R->Arguments[I] == DataValue + I, 13);
  }
  if (Mode == NestedMode) {
    if (R->Code == SoftwareCode) {
      require(Depth == 0, 14);
      ++Depth;
      RaiseException(NestedCode, 0, 0, 0);
      require(Depth == 1 && Calls == 2, 15);
      --Depth;
    } else
      require(R->Code == NestedCode && Depth == 1, 16);
  }
  if (Mode == ReadMode || Mode == WriteMode || Mode == ContextMode) {
    require(R->Code == AccessViolation && R->Count == 2, 17);
    require(R->Arguments[0] == (Mode == WriteMode), 18);
    require(R->Arguments[1] ==
                (Mode == WriteMode ? (U64)WritePage : FaultAddress),
            19);
    require((U64)R->Address == *(U64 *)(C + ContextPC), 20);
    if (Mode == ReadMode) {
      require(R->Address == (void *)&ReadFault, 21);
      *(U64 *)(C + ContextPointer) = (U64)&GoodData;
    } else if (Mode == WriteMode) {
      require(R->Address == (void *)&WriteFault, 22);
      U32 Old;
      require(VirtualProtect(WritePage, PageSize, PageReadWrite, &Old) &&
                  Old == PageReadOnly,
              23);
    } else {
      require(R->Address == (void *)&ContextFaultSite, 24);
      require(*(U64 *)(C + ContextVector) == VectorInitialLow &&
                  *(U64 *)(C + ContextVector + sizeof(U64)) ==
                      VectorInitialHigh,
              25);
      *(U64 *)(C + ContextPC) = (U64)&ContextResume;
      *(U64 *)(C + ContextResult) = DataValue;
      *(U64 *)(C + ContextVector) = VectorLow;
      *(U64 *)(C + ContextVector + sizeof(U64)) = VectorHigh;
      *(U32 *)(C + ContextControl) = ControlValue;
      *(U32 *)(C + ContextControlCopy) = ControlValue;
    }
  }
#if defined(_M_X64) || defined(__x86_64__)
  if (Mode == DivideMode) {
    require(R->Code == DivideByZero && R->Count == 0 &&
                R->Address == (void *)&DivideFaultSite,
            26);
    *(U64 *)(C + ContextPointer) = 2;
  }
#endif
  if (Mode == LoaderMode) {
    void *DLL = LoadLibraryA(LibraryFile);
    require(DLL != 0 && Notifications == 1, 27);
    require(FreeLibrary(DLL) && Notifications == 2, 28);
  }
  if (Mode == ExitMode)
    complete();
  if (Mode == UnhandledMode)
    return ContinueSearch;
  if (Mode == DispositionMode)
    return 1;
  if (Mode == FlagsMode)
    *(U32 *)(C + ContextFlags) = 0;
  if (Mode == PCMode)
    *(U64 *)(C + ContextPC) = 0;
  if (Mode == StackMode)
    *(U64 *)(C + ContextSP) = 0;
  if (Mode == PrivilegedMode)
    *(U64 *)(C + ContextPrivileged) ^= 1;
  if (Mode == PointersMode)
    P->Context = 0;
  if (Mode == FPMode) {
#if defined(_M_X64) || defined(__x86_64__)
    *(U32 *)(C + ContextControl) = 0;
    *(U32 *)(C + ContextControlCopy) = 0;
#else
    *(U32 *)(C + ContextControl) |= 1;
#endif
  }
  if (Mode == RecursiveMode)
    RaiseException(SoftwareCode, 0, 0, 0);
  return ContinueExecution;
}
U32 entry(void) {
  Mode = SoftwareMode;
  for (const U16 *P = GetCommandLineW(); *P; ++P)
    if (*P == ModePrefix && P[1])
      Mode = P[1];
  if (Mode == OrderMode) {
    void *A = AddVectoredExceptionHandler(0, one);
    void *B = AddVectoredExceptionHandler(2, two);
    void *C = AddVectoredExceptionHandler(0, three);
    require(A && B && C && A != B && B != C, 29);
    RaiseException(SoftwareCode, 0, 0, 0);
    require(Trace == OrderTrace, 30);
    require(RemoveVectoredExceptionHandler(A) &&
                RemoveVectoredExceptionHandler(B) &&
                RemoveVectoredExceptionHandler(C),
            31);
    Trace = 0;
    A = AddVectoredExceptionHandler(0, one);
    B = AddVectoredExceptionHandler(0, one);
    C = AddVectoredExceptionHandler(0, three);
    require(A && B && C && A != B, 32);
    RaiseException(SoftwareCode, 0, 0, 0);
    require(Trace == DuplicateTrace, 33);
    complete();
  }
  if (Mode == MutationMode) {
    SelfHandle = AddVectoredExceptionHandler(0, mutate);
    NextHandle = AddVectoredExceptionHandler(0, victim);
    require(SelfHandle && NextHandle && AddVectoredExceptionHandler(0, four),
            34);
    RaiseException(SoftwareCode, 0, 0, 0);
    require(Trace == MutationTrace, 35);
    Trace = 0;
    RaiseException(SoftwareCode, 0, 0, 0);
    require(Trace == NextMutationTrace, 36);
    complete();
  }
  void *Handle = AddVectoredExceptionHandler(0, Mode == HandlerMode ? (Handler)1
                                                                    : resolve);
  require(Handle != 0, 37);
  if (Mode == HandlerLimitMode)
    for (U32 I = 0; I < ModelHandlerLimit; ++I)
      AddVectoredExceptionHandler(0, resolve);
  if (Mode == ReadMode)
    require(ReadFault((const void *)FaultAddress) == DataValue, 38);
  else if (Mode == WriteMode) {
    WritePage = VirtualAlloc(0, PageSize, MemReserveCommit, PageReadWrite);
    U32 Old;
    require(WritePage != 0 &&
                VirtualProtect(WritePage, PageSize, PageReadOnly, &Old),
            39);
    WriteFault(WritePage, DataValue);
    require(*(U64 *)WritePage == DataValue && Calls == 1, 40);
    require(VirtualFree(WritePage, 0, MemRelease), 41);
  } else if (Mode == ContextMode) {
    U64 Vector[2];
    U64 Result = ContextFault((const void *)FaultAddress, Vector);
    require(Result == DataValue && Vector[0] == VectorLow &&
                Vector[1] == VectorHigh && Calls == 1,
            42);
    RaiseException(ContextProbeCode, 0, 0, 0);
    require(Calls == 2, 48);
#if defined(_M_X64) || defined(__x86_64__)
  } else if (Mode == DivideMode) {
    require(DivideFault(0) == 8 && Calls == 1, 43);
#endif
  } else {
    U64 Arguments[MaxParameters + 1];
    for (U32 I = 0; I < MaxParameters + 1; ++I)
      Arguments[I] = DataValue + I;
    SetLastError(LastErrorSeed);
    RaiseException(SoftwareCode,
                   Mode == NoncontinuableMode ? 1
                   : Mode == RaiseFlagsMode   ? 2
                                              : 0,
                   Mode == ArgumentsMode ? MaxParameters + 1 : MaxParameters,
                   Arguments);
    if (Mode == SoftwareMode) {
      require(GetLastError() == LastErrorSeed, 44);
      RaiseException(SoftwareCode, 0, (U32)-1, 0);
      require(Calls == 2, 45);
    }
  }
  require(RemoveVectoredExceptionHandler(Handle), 46);
  complete();
  return FailureStatus;
}

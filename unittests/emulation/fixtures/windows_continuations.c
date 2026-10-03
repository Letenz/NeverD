//===- windows_continuations.c - Original Windows VCH executable ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef unsigned char U8;
#define NEVERD_VEH_VALUE(Name, Value) enum { Name = Value };
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
#undef NEVERD_VEH_VALUE
#define NEVERD_VCH_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_VCH_MODE(Name, Value) enum { Name = Value };
#define NEVERD_VCH_TEXT(Name, Text) static const char Name[] = Text;
#include "WindowsContinuationCases.def"
#undef NEVERD_VCH_TEXT
#undef NEVERD_VCH_MODE
#undef NEVERD_VCH_VALUE

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
__declspec(dllimport) void *AddVectoredExceptionHandler(U32, Handler);
__declspec(dllimport) U32 RemoveVectoredExceptionHandler(void *);
__declspec(dllimport) void *AddVectoredContinueHandler(U32, Handler);
__declspec(dllimport) U32 RemoveVectoredContinueHandler(void *);
__declspec(dllimport) void RaiseException(U32, U32, U32, const U64 *);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
extern U64 ReadFault(const void *);
extern U64 ContextFault(const void *, U64 *);
extern void ContextResume(void);
__declspec(align(16))
const U64 VectorSeed[] = {VectorInitialLow, VectorInitialHigh};
static U32 Mode, Calls, Continues, Depth, Notifications;
static U64 Trace, GoodData = DataValue, OtherData = AlternateData;
static void *SelfHandle, *NextHandle, *AddedFirst, *AddedLast;
static Record *OriginalRecord;
static U8 *OriginalContext;

static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U64 Observation[] = {Site, Mode, Trace, Calls, Continues};
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), Observation, sizeof(Observation),
            &Written, 0);
  ExitProcess(FailureStatus);
}
static void complete(void) {
  U64 Result[ObservationWords] = {Mode, Trace, Calls, Continues};
  U32 Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Result, sizeof(Result),
                    &Written, 0) &&
              Written == sizeof(Result),
          1);
  ExitProcess(CompletionStatus);
}
static void trace(U32 ID) { Trace = (Trace << TraceShift) | ID; }
static U32 one(Pointers *P) {
  ++Continues;
  trace(1);
  return ContinueSearch;
}
static U32 two(Pointers *P) {
  ++Continues;
  trace(2);
  return Mode == ShortCircuitMode ? ContinueExecution : ContinueSearch;
}
static U32 three(Pointers *P) {
  ++Continues;
  trace(3);
  return ContinueSearch;
}
static U32 four(Pointers *P) {
  ++Continues;
  trace(4);
  return ContinueSearch;
}
static U32 mutate(Pointers *P) {
  ++Continues;
  trace(1);
  require(RemoveVectoredContinueHandler(SelfHandle) &&
              RemoveVectoredContinueHandler(NextHandle),
          2);
  AddedFirst = AddVectoredContinueHandler(1, two);
  AddedLast = AddVectoredContinueHandler(0, four);
  require(AddedFirst && AddedLast, 3);
  return ContinueSearch;
}
static U32 handle(Pointers *P) {
  ++Calls;
  trace(ExceptionTrace);
  if (Mode == NoncontinuableMode)
    require(P->Record->Flags == (Noncontinuable | SoftwareOriginate), 26);
  if (!Depth && P->Record->Code != AttachCode &&
      P->Record->Code != DetachCode) {
    OriginalRecord = P->Record;
    OriginalContext = P->Context;
  }
  if (Mode == AddMode)
    require(AddVectoredContinueHandler(0, one) != 0, 4);
  if (Mode == RemoveMode)
    require(RemoveVectoredContinueHandler(SelfHandle), 5);
  if (Mode == ContextMode)
    *(U64 *)(P->Context + ContextPointer) = (U64)&GoodData;
  if (Mode == FinalContextMode && P->Record->Code == AccessViolation)
    *(U64 *)(P->Context + ContextPC) = 0;
  return ContinueExecution;
}
static U32 resume(Pointers *P) {
  ++Continues;
  if (Mode == NestedMode && P->Record->Code == NestedCode) {
    trace(2);
    require(Depth == 1, 6);
    return ContinueSearch;
  }
  if (Mode == LoaderMode &&
      (P->Record->Code == AttachCode || P->Record->Code == DetachCode)) {
    trace(P->Record->Code == AttachCode ? 2 : 3);
    ++Notifications;
    return ContinueSearch;
  }
  trace(1);
  require(P->Record == OriginalRecord && P->Context == OriginalContext, 7);
  if (Mode == NoncontinuableMode)
    require(P->Record->Code == SoftwareCode &&
                P->Record->Flags == (Noncontinuable | SoftwareOriginate),
            27);
  if (Mode == ContextMode) {
    require(P->Record->Code == AccessViolation && P->Record->Flags == 0 &&
                *(U64 *)(P->Context + ContextPointer) == (U64)&GoodData,
            8);
    *(U64 *)(P->Context + ContextPointer) = (U64)&OtherData;
  }
  if (Mode == FinalContextMode) {
    if (P->Record->Code == AccessViolation) {
      require(*(U64 *)(P->Context + ContextPC) == 0, 9);
      *(U64 *)(P->Context + ContextPC) = (U64)&ContextResume;
      *(U64 *)(P->Context + ContextResult) = AlternateData;
      *(U64 *)(P->Context + ContextVector) = VectorLow;
      *(U64 *)(P->Context + ContextVector + sizeof(U64)) = VectorHigh;
      *(U32 *)(P->Context + ContextControl) = ControlValue;
      *(U32 *)(P->Context + ContextControlCopy) = ControlValue;
    } else {
      require(P->Record->Code == ContextProbeCode &&
                  *(U32 *)(P->Context + ContextControl) == ControlValue,
              10);
    }
  }
  if (Mode == NestedMode) {
    require(!Depth && Calls == 1, 11);
    ++Depth;
    RaiseException(NestedCode, 0, 0, 0);
    --Depth;
    require(Calls == 2 && Continues == 2 && P->Record == OriginalRecord &&
                P->Context == OriginalContext &&
                P->Record->Code == SoftwareCode,
            12);
    trace(3);
  }
  if (Mode == LoaderMode) {
    void *DLL = LoadLibraryA(LibraryFile);
    require(DLL != 0 && Notifications == 1, 13);
    require(FreeLibrary(DLL) && Notifications == 2, 14);
  }
  if (Mode == ExitMode)
    complete();
  if (Mode == PCMode)
    *(U64 *)(P->Context + ContextPC) = 0;
  if (Mode == StackMode)
    *(U64 *)(P->Context + ContextSP) = 0;
  if (Mode == PointersMode)
    P->Context = 0;
  if (Mode == DispositionMode)
    return 1;
  if (Mode == RecursiveMode)
    RaiseException(SoftwareCode, 0, 0, 0);
  return ContinueSearch;
}
U32 entry(void) {
  Mode = OrderMode;
  for (const U16 *P = GetCommandLineW(); *P; ++P)
    if (*P == ModePrefix && P[1])
      Mode = P[1];
  void *Exception = 0;
  if (Mode != UnhandledMode) {
    Exception = AddVectoredExceptionHandler(0, handle);
    require(Exception != 0, 15);
  }
  if (Mode == OrderMode || Mode == ShortCircuitMode || Mode == DuplicateMode) {
    void *A = AddVectoredContinueHandler(0, one);
    void *B = AddVectoredContinueHandler(Mode == OrderMode,
                                         Mode == DuplicateMode ? one : two);
    void *C = AddVectoredContinueHandler(0, three);
    require(A && B && C && A != B && B != C, 16);
    RaiseException(SoftwareCode, 0, 0, 0);
    require(RemoveVectoredContinueHandler(A) &&
                RemoveVectoredContinueHandler(B) &&
                RemoveVectoredContinueHandler(C),
            17);
    complete();
  }
  if (Mode == MutationMode) {
    SelfHandle = AddVectoredContinueHandler(0, mutate);
    NextHandle = AddVectoredContinueHandler(0, two);
    require(SelfHandle && NextHandle && AddVectoredContinueHandler(0, three),
            18);
    RaiseException(SoftwareCode, 0, 0, 0);
    // Native registrations may reuse removed tokens. Do not remove a new
    // live registration through an aliased retired token.
    if (SelfHandle != AddedFirst && SelfHandle != AddedLast)
      require(!RemoveVectoredContinueHandler(SelfHandle), 19);
    if (NextHandle != AddedFirst && NextHandle != AddedLast)
      require(!RemoveVectoredContinueHandler(NextHandle), 28);
    RaiseException(SoftwareCode, 0, 0, 0);
    complete();
  }
  if (Mode != AddMode) {
    SelfHandle = AddVectoredContinueHandler(0, Mode == HandlerMode ? (Handler)1
                                                                   : resume);
    require(SelfHandle != 0, 20);
  }
  if (Mode == HandleMode)
    require(!RemoveVectoredContinueHandler(Exception) &&
                !RemoveVectoredExceptionHandler(SelfHandle),
            21);
  if (Mode == LimitMode)
    for (U32 I = 0; I < ModelHandlerLimit; ++I) {
      if (I & 1)
        AddVectoredExceptionHandler(0, handle);
      else
        AddVectoredContinueHandler(0, resume);
    }
  if (Mode == ExitMode)
    require(AddVectoredContinueHandler(0, two) != 0, 22);
  if (Mode == ContextMode)
    require(ReadFault((const void *)FaultAddress) == AlternateData, 23);
  else if (Mode == FinalContextMode) {
    U64 Vector[2];
    require(ContextFault((const void *)FaultAddress, Vector) == AlternateData &&
                Vector[0] == VectorLow && Vector[1] == VectorHigh,
            24);
    RaiseException(ContextProbeCode, 0, 0, 0);
  } else
    RaiseException(SoftwareCode,
                   Mode == NoncontinuableMode ? Noncontinuable : 0, 0, 0);
  if (Mode == HandleMode)
    require(RemoveVectoredExceptionHandler(Exception) &&
                RemoveVectoredContinueHandler(SelfHandle) &&
                !RemoveVectoredExceptionHandler(Exception) &&
                !RemoveVectoredContinueHandler(SelfHandle),
            25);
  complete();
  return FailureStatus;
}

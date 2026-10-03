//===- windows_seh.c - Original x64 user C exception oracle --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef unsigned char U8;
#define NEVERD_USER_SEH_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_USER_SEH_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_USER_SEH_CASE(Name, ID, Argument, Trace)                        \
  enum { Name##Mode = ID };
#define NEVERD_USER_SEH_NEGATIVE(Name, ID, Argument, Diagnostic)               \
  enum { Name##Mode = ID };
#define NEVERD_USER_SEH_SECONDARY(Name, ID, Argument, Trace)                   \
  enum { Name##Mode = ID };
#define NEVERD_USER_SEH_TERMINAL(Name, ID, Argument, Status, Trace)            \
  enum { Name##Mode = ID };
#define NEVERD_USER_SEH_ASM(Text) __asm__(Text);
#include "WindowsSEHCases.def"
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
typedef void (*TraceCall)(U32);
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) void RaiseException(U32, U32, U32, const U64 *);
__declspec(dllimport) void *AddVectoredExceptionHandler(U32, Handler);
__declspec(dllimport) void *AddVectoredContinueHandler(U32, Handler);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) void *GetModuleHandleW(const U16 *);
__declspec(dllimport) int VirtualProtect(void *, U64, U32, U32 *);
unsigned long _exception_code(void);
void *_exception_info(void);
int _abnormal_termination(void);
extern U64 ReadFault(const void *);
static U64 Mode, Trace, GoodData = DataValue;
static void *Library;
static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}
static void trace(U32 ID) {
  Trace = (Trace << TraceShift) | ID;
  if (Mode == SecondaryVectoredMode || Mode == SecondaryRedirectMode) {
    const U64 Value = ID;
    U32 Written;
    require(WriteFile(GetStdHandle(StdoutSelector), &Value, sizeof(Value),
                      &Written, 0) &&
                Written == sizeof(Value),
            StreamOutputSite);
  }
}
static U32 SecondaryCount;
static U64 OriginalPC, OriginalSP;
static U8 OriginalContext[ContextSize];
static void observe(Pointers *P, U32 Site) {
  require(P && P->Record && P->Context, RecordPointersSite);
  U64 PC = *(U64 *)(P->Context + ContextPC);
  U64 SP = *(U64 *)(P->Context + ContextSP);
  if (!OriginalPC) {
    OriginalPC = PC;
    OriginalSP = SP;
    for (U32 I = 0; I < ContextSize; ++I)
      OriginalContext[I] = P->Context[I];
  }
  require(P->Record->Flags == NoncontinuableFlags && !P->Record->Count &&
              !P->Record->Nested,
          RecordFieldsSite);
  if (P->Record->Code == SoftwareCode) {
    require(PC == (U64)P->Record->Address && PC == OriginalPC &&
                SP == OriginalSP,
            OriginalContextSite);
  } else {
    require(P->Record->Code == NoncontinuableCode, SecondaryCodeSite);
    if (Mode == SecondaryRedirectMode && Site == ContinueTrace)
      require(PC == OriginalPC && SP == OriginalSP, RestoredContextSite);
    else
      require(PC == (U64)P->Record->Address && PC != OriginalPC &&
                  SP < OriginalSP,
              SecondaryContextSite);
  }
}
static U32 secondaryVectored(Pointers *P) {
  observe(P, VectoredTrace);
  trace(VectoredTrace);
  if (Mode == SecondaryRedirectMode && P->Record->Code == NoncontinuableCode) {
    for (U32 I = 0; I < ContextSize; ++I)
      P->Context[I] = OriginalContext[I];
    return -1;
  }
  if (Mode == SecondaryVectoredMode && P->Record->Code == NoncontinuableCode &&
      !SecondaryCount++)
    return -1;
  return 0;
}
static U32 secondaryContinued(Pointers *P) {
  observe(P, ContinueTrace);
  trace(ContinueTrace);
  return 0;
}
static int secondaryFilter(Pointers *P) {
  observe(P, FilterTrace);
  trace(FilterTrace);
  if (P->Record->Code == SoftwareCode)
    return -1;
  require(P->Record->Code == NoncontinuableCode, SecondaryCodeSite);
  if (Mode == SecondaryRepeatMode && !SecondaryCount++)
    return -1;
  return Mode != SecondarySearchMode;
}
__declspec(noinline) static void secondaryUnwind(void) {
  __try {
    RaiseException(SoftwareCode, RaiseNoncontinuable, 0, 0);
  } __finally {
    trace(_abnormal_termination() ? FinallyTrace : LocalHandlerTrace);
  }
}
__declspec(noinline) static void secondaryInner(void) {
  __try {
    if (Mode == SecondaryUnwindMode)
      secondaryUnwind();
    else if (Mode == SecondaryDLLMode || Mode == SecondaryDLLHandlerMode) {
      void (*Call)(TraceCall, int (*)(Pointers *), U32) = GetProcAddress(
          Library, Mode == SecondaryDLLMode ? DLLRaiseName : DLLCatchName);
      require(Call != 0, SecondaryExportSite);
      Call(trace, secondaryFilter, RaiseNoncontinuable);
    } else
      RaiseException(SoftwareCode, RaiseNoncontinuable, 0, 0);
    require(Mode == SecondaryRedirectMode || Mode == SecondaryDLLHandlerMode,
            23);
    trace(ResumeTrace);
  } __except (secondaryFilter(_exception_info())) {
    require(_exception_code() == NoncontinuableCode, SecondaryCatchSite);
    trace(HandlerTrace);
  }
}
__declspec(noinline) static void secondaryCleanup(void) {
  __try {
    secondaryInner();
  } __finally {
    trace(_abnormal_termination() ? FinallyTrace : LocalHandlerTrace);
  }
}
static void secondary(void) {
  require(AddVectoredExceptionHandler(1, secondaryVectored) != 0,
          SecondaryHandlerSite);
  require(AddVectoredContinueHandler(1, secondaryContinued) != 0,
          SecondaryContinueSite);
  if (Mode == SecondaryDLLMode || Mode == SecondaryDLLHandlerMode) {
    Library = LoadLibraryA(LibraryFile);
    require(Library != 0, SecondaryLoadSite);
  }
  __try {
    if (Mode == SecondaryFinallyMode)
      secondaryCleanup();
    else
      secondaryInner();
    trace(ResumeTrace);
  } __except (1) {
    require(_exception_code() == NoncontinuableCode, SecondaryCatchSite);
    trace(LocalHandlerTrace);
  }
  if (Library)
    require(FreeLibrary(Library), SecondaryFreeSite);
}
static U32 vectored(Pointers *P) {
  trace(VectoredTrace);
  return 0;
}
static U32 continued(Pointers *P) {
  trace(ContinueTrace);
  return 0;
}
__declspec(noinline) static void raise(void) {
  RaiseException(SoftwareCode,
                 Mode == NoncontinuableCatchMode || Mode == NoncontinuableMode,
                 0, 0);
}
static int filter(Pointers *P) {
  require(P && P->Record && P->Context, 1);
  if (Mode == NestedFilterMode && P->Record->Code == SoftwareCode) {
    trace(1);
    RaiseException(NestedCode, 0, 0, 0);
    require(0, 2);
  }
  trace(FilterTrace);
  require(
      P->Record->Code == SoftwareCode || P->Record->Code == NestedCode ||
          P->Record->Code == AccessViolation ||
          (Mode == NoncontinuableMode && P->Record->Code == NoncontinuableCode),
      3);
  if (Mode == LocalNestedMode) {
    __try {
      RaiseException(NestedCode, 0, 0, 0);
    } __except (1) {
      require(_exception_code() == NestedCode, 4);
      trace(LocalHandlerTrace);
    }
  }
  if (Mode == RepairFaultMode) {
    require(P->Record->Code == AccessViolation && P->Record->Count == 2 &&
                P->Record->Arguments[1] == FaultAddress,
            5);
    *(U64 *)(P->Context + ContextRCX) = (U64)&GoodData;
    return -1;
  }
  if (Mode == ContinueMode || Mode == NoncontinuableMode)
    return -1;
  if (Mode == MetadataMode) {
    U8 *Base = GetModuleHandleW(0);
    U8 *Table = Base + *(U32 *)(Base + *(U32 *)(Base + DOSNTOffset) +
                                ExceptionDirectoryOffset);
    U32 Old;
    require(VirtualProtect(Table, sizeof(U32), PageReadWrite, &Old), 6);
    *Table ^= 1;
  }
  if (Mode == RetiredMode)
    require(FreeLibrary(Library), 7);
  return 1;
}
static int search(void) {
  trace(1);
  return 0;
}
__declspec(noinline) static void cleanup(void) {
  __try {
    raise();
  } __finally {
    require(_abnormal_termination(), 8);
    trace(FinallyTrace);
    if (Mode == CollidedFinallyMode) {
      RaiseException(NestedCode, 0, 0, 0);
      require(0, 9);
    }
  }
}
void entry(void) {
  const U16 *Command = GetCommandLineW();
  while (*Command) {
    if (*Command++ == ModeMarker) {
      Mode = *Command;
      break;
    }
  }
  require(Mode != 0, 10);
  if (Mode == SecondarySameMode || Mode == SecondarySearchMode ||
      Mode == SecondaryFinallyMode || Mode == SecondaryVectoredMode ||
      Mode == SecondaryRepeatMode || Mode == SecondaryRedirectMode ||
      Mode == SecondaryUnwindMode || Mode == SecondaryDLLMode ||
      Mode == SecondaryDLLHandlerMode) {
    secondary();
    const U64 Output[] = {Mode, Trace};
    U32 Written;
    require(WriteFile(GetStdHandle(StdoutSelector), Output, sizeof(Output),
                      &Written, 0),
            SecondaryOutputSite);
    ExitProcess(CompletionStatus);
  }
  if (Mode == VectoredFirstMode)
    require(AddVectoredExceptionHandler(1, vectored) != 0, 11);
  require(AddVectoredContinueHandler(1, continued) != 0, 12);
  if (Mode == CatchMode) {
    __try {
      raise();
    } __except (1) {
      require(_exception_code() == SoftwareCode, 13);
      trace(HandlerTrace);
    }
  } else if (Mode == SearchMode) {
    __try {
      __try {
        raise();
      } __except (search()) {
        require(0, 14);
      }
    } __except (filter(_exception_info())) {
      trace(HandlerTrace);
    }
  } else {
    void (*DLLCall)(TraceCall, int (*)(Pointers *), U32) = 0;
    if (Mode == CrossImageMode || Mode == DLLHandlerMode ||
        Mode == RetiredMode) {
      Library = LoadLibraryA(LibraryFile);
      require(Library != 0, 15);
      DLLCall = (void (*)(TraceCall, int (*)(Pointers *), U32))GetProcAddress(
          Library, Mode == DLLHandlerMode ? DLLCatchName : DLLRaiseName);
      require(DLLCall != 0, 16);
    }
    __try {
      if (Mode == FinallyMode || Mode == CollidedFinallyMode)
        cleanup();
      else if (Mode == ReadFaultMode || Mode == RepairFaultMode)
        require(ReadFault((void *)FaultAddress) == DataValue, 17);
      else if (DLLCall)
        DLLCall(trace, filter, 0);
      else
        raise();
      trace(ResumeTrace);
    } __except (filter(_exception_info())) {
      require(
          _exception_code() ==
              ((Mode == NestedFilterMode || Mode == CollidedFinallyMode)
                   ? NestedCode
                   : (Mode == ReadFaultMode ? AccessViolation : SoftwareCode)),
          18);
      trace(HandlerTrace);
    }
    if (Library)
      require(FreeLibrary(Library), 19);
  }
  const U64 Output[] = {Mode, Trace};
  U32 Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Output, sizeof(Output),
                    &Written, 0) &&
              Written == sizeof(Output),
          20);
  ExitProcess(CompletionStatus);
}

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
static void trace(U32 ID) { Trace = (Trace << TraceShift) | ID; }
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
  require(P->Record->Code == SoftwareCode || P->Record->Code == NestedCode ||
              P->Record->Code == AccessViolation,
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
    void (*DLLCall)(TraceCall, int (*)(Pointers *)) = 0;
    if (Mode == CrossImageMode || Mode == DLLHandlerMode ||
        Mode == RetiredMode) {
      Library = LoadLibraryA(LibraryFile);
      require(Library != 0, 15);
      DLLCall = (void (*)(TraceCall, int (*)(Pointers *)))GetProcAddress(
          Library, Mode == DLLHandlerMode ? DLLCatchName : DLLRaiseName);
      require(DLLCall != 0, 16);
    }
    __try {
      if (Mode == FinallyMode || Mode == CollidedFinallyMode)
        cleanup();
      else if (Mode == ReadFaultMode || Mode == RepairFaultMode)
        require(ReadFault((void *)FaultAddress) == DataValue, 17);
      else if (DLLCall)
        DLLCall(trace, filter);
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

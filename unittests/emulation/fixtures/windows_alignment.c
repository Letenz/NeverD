//===- windows_alignment.c - Native Windows aligned SSE observations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <windows.h>

typedef unsigned long long U64;
#define NEVERD_WINDOWS_ALIGNMENT_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_WINDOWS_ALIGNMENT_WIDE(Name, Value)                             \
  static const U64 Name = Value;
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_WIDE
#undef NEVERD_WINDOWS_ALIGNMENT_VALUE

enum Scenario {
#define NEVERD_WINDOWS_ALIGNMENT_SCENARIO(Name) Scenario##Name,
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_SCENARIO
  ScenarioCount
};
enum Field {
#define NEVERD_WINDOWS_ALIGNMENT_FIELD(Name) Field##Name,
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_FIELD
  FieldCount
};
#define NEVERD_WINDOWS_ALIGNMENT_OPERATION(Name, ...)                          \
  extern void Name(const void *, void *);                                      \
  extern void Name##Fault(void), Name##Resume(void);
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_OPERATION

typedef struct {
  void (*Run)(const void *, void *);
  void (*Fault)(void);
  void (*Resume)(void);
} Operation;
static const Operation Operations[] = {
#define NEVERD_WINDOWS_ALIGNMENT_OPERATION(Name, ...)                          \
  {Name, Name##Fault, Name##Resume},
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_OPERATION
};
_Alignas(VectorBytes) const U64 AlignmentSeed[] = {VectorLow, VectorHigh};
static U64 Record[FieldCount];
static const Operation *Current;
static unsigned Calls;

static void require(int Valid, unsigned Site) {
  if (Valid)
    return;
  DWORD Written;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), &Site, sizeof(Site), &Written, 0);
  ExitProcess(FailureStatus);
}

static LONG CALLBACK observe(EXCEPTION_POINTERS *Pointers) {
  const EXCEPTION_RECORD *E = Pointers->ExceptionRecord;
  CONTEXT *C = Pointers->ContextRecord;
  require(!Calls++ && (U64)E->ExceptionAddress == (U64)Current->Fault &&
              C->Rip == (U64)Current->Fault && !E->ExceptionRecord &&
              E->NumberParameters <= EXCEPTION_MAXIMUM_PARAMETERS,
          SiteContext);
  Record[FieldCode] = E->ExceptionCode;
  Record[FieldFlags] = E->ExceptionFlags;
  Record[FieldParameterCount] = E->NumberParameters;
  for (unsigned I = 0; I < EXCEPTION_MAXIMUM_PARAMETERS; ++I)
    Record[FieldParameter0 + I] =
        I < E->NumberParameters ? E->ExceptionInformation[I] : 0;
  Record[FieldFaultPC] = (U64)Current->Fault;
  Record[FieldExceptionPC] = (U64)E->ExceptionAddress;
  Record[FieldContextPC] = C->Rip;
  Record[FieldContextOperand] = C->Rcx;
  Record[FieldContextFlags] = C->EFlags;
  Record[FieldVectorLow] = C->Xmm0.Low;
  Record[FieldVectorHigh] = C->Xmm0.High;
  require(C->Rcx == Record[FieldOperand] && C->Xmm0.Low == VectorLow &&
              (U64)C->Xmm0.High == VectorHigh,
          SiteContext);
  // Observe the original fault, then skip only that known leaf instruction.
  // No guessed exception code or OS address convention is an acceptance rule.
  C->Rip = (U64)Current->Resume;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void entry(void) {
  SYSTEM_INFO Host;
  GetNativeSystemInfo(&Host);
  require(Host.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 &&
              Host.dwPageSize == PageBytes,
          SiteHost);
  unsigned char *Memory =
      VirtualAlloc(0, ReservedPages * PageBytes, MEM_RESERVE, PAGE_NOACCESS);
  require(Memory && VirtualAlloc(Memory, CommittedPages * PageBytes, MEM_COMMIT,
                                 PAGE_READWRITE) == Memory,
          SiteAllocation);
  for (unsigned I = 0; I < PageBytes; ++I)
    Memory[I] = (unsigned char)(I ^ MemorySeed);
  DWORD Previous;
  require(
      VirtualProtect(Memory + PageBytes, PageBytes, PAGE_NOACCESS, &Previous),
      SiteProtection);
  void *Handler = AddVectoredExceptionHandler(1, observe);
  require(Handler != 0, SiteHandler);
  for (unsigned Op = 0; Op < sizeof(Operations) / sizeof(*Operations); ++Op) {
    Current = &Operations[Op];
    for (unsigned Case = 0; Case < ScenarioCount; ++Case) {
      U64 Operand = (U64)Memory + 1;
      DWORD Protection = PAGE_READWRITE;
      switch (Case) {
      case ScenarioReadOnly:
        Protection = PAGE_READONLY;
        break;
      case ScenarioNoAccess:
        Protection = PAGE_NOACCESS;
        break;
      case ScenarioCrossPage:
        Operand = (U64)Memory + PageBytes - 1;
        break;
      case ScenarioReserved:
        Operand = (U64)Memory + CommittedPages * PageBytes + 1;
        break;
      case ScenarioNoncanonical:
        Operand = Noncanonical;
        break;
      case ScenarioWrapped:
        Operand = WrappedAddress;
        break;
      case ScenarioAlignedNoAccess:
        Operand = (U64)Memory;
        Protection = PAGE_NOACCESS;
        break;
      }
      require(VirtualProtect(Memory, PageBytes, Protection, &Previous),
              SiteProtection);
      Calls = 0;
      Record[FieldOperation] = Op;
      Record[FieldScenario] = Case;
      Record[FieldOperand] = Operand;
      U64 Output[VectorBytes / sizeof(U64)];
      Current->Run((void *)Operand, Output);
      require(Calls == 1, SiteHandler);
      require(Output[0] == VectorLow && Output[1] == VectorHigh, SiteVector);
      require(VirtualProtect(Memory, PageBytes, PAGE_READWRITE, &Previous),
              SiteProtection);
      for (unsigned I = 0; I < PageBytes; ++I)
        require(Memory[I] == (unsigned char)(I ^ MemorySeed), SiteMemory);
      DWORD Written;
      require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Record, sizeof(Record),
                        &Written, 0) &&
                  Written == sizeof(Record),
              SiteOutput);
    }
  }
  require(RemoveVectoredExceptionHandler(Handler), SiteCleanup);
  require(VirtualFree(Memory, 0, MEM_RELEASE), SiteCleanup);
  ExitProcess(0);
}

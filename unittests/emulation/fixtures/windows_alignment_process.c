//===- windows_alignment_process.c - Original Windows SSE recovery --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <stdbool.h>

typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned char U8;
#define NEVERD_WINDOWS_ALIGNMENT_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_WINDOWS_ALIGNMENT_WIDE(Name, Value)                             \
  static const U64 Name = Value;
#define NEVERD_ALIGNMENT_PROCESS_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_VEH_CONTEXT_X64(Name, Value) enum { Name = Value };
#include "WindowsAlignmentCases.def"
#include "WindowsAlignmentProcessCases.def"
#include "WindowsExceptionCases.def"
#undef NEVERD_VEH_CONTEXT_X64
#undef NEVERD_ALIGNMENT_PROCESS_VALUE
#undef NEVERD_WINDOWS_ALIGNMENT_WIDE
#undef NEVERD_WINDOWS_ALIGNMENT_VALUE

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
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) void *AddVectoredExceptionHandler(U32, Handler);
__declspec(dllimport) U32 RemoveVectoredExceptionHandler(void *);
__declspec(dllimport) void *VirtualAlloc(void *, U64, U32, U32);
__declspec(dllimport) int VirtualProtect(void *, U64, U32, U32 *);
__declspec(dllimport) int VirtualFree(void *, U64, U32);

enum Scenario {
#define NEVERD_WINDOWS_ALIGNMENT_SCENARIO(Name) Scenario##Name,
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_SCENARIO
  ScenarioCount
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
  U32 Store;
  U64 Low, High;
} Operation;
static const Operation Operations[] = {
#define NEVERD_ALIGNMENT_PROCESS_RESULT(Name, Store, Low, High)                \
  {Name, Name##Fault, Name##Resume, Store, Low, High},
#include "WindowsAlignmentProcessCases.def"
#undef NEVERD_ALIGNMENT_PROCESS_RESULT
};
__declspec(align(16)) const U64 AlignmentSeed[] = {VectorLow, VectorHigh};
static const Operation *Current;
static U8 *Memory;
static U64 Operand;
static U32 Calls, Op, Case, Repair;
static U32 CompletedScenarios, CompletedRetries, ValidatedFaults;
static U64 ObservedVector[VectorBytes / sizeof(U64)];

static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U64 Failure[] = {
      Site, Op, Case, Repair, Calls, ObservedVector[0], ObservedVector[1]};
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), Failure, sizeof(Failure), &Written,
            0);
  ExitProcess(FailureStatus);
}
static void writeOutput(const void *Bytes, U32 Size) {
  U32 Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Bytes, Size, &Written, 0) &&
              Written == Size,
          SiteOutput);
}
static U32 observe(Pointers *Pointers) {
  const Record *R = Pointers->Record;
  U8 *C = Pointers->Context;
  require(!Calls++ && R && C && R->Code == (U32)AccessViolation && !R->Flags &&
              !R->Nested && R->Count == AccessViolationParameters &&
              R->Address == (void *)Current->Fault &&
              *(U64 *)(C + ContextPC) == (U64)Current->Fault &&
              *(U64 *)(C + ContextPointer) == Operand &&
              (*(U32 *)(C + ContextProcessorFlags) & ResumeFlag) &&
              *(U64 *)(C + ContextVector) == VectorLow &&
              *(U64 *)(C + ContextVector + sizeof(U64)) == VectorHigh,
          SiteContext);
  // Native Windows records [read, UINT64_MAX] for these aligned-SSE #GPs,
  // including stores. A real aligned page fault keeps its access and address.
  const int PageFault = Case == ScenarioAlignedNoAccess;
  require(R->Arguments[0] == (PageFault ? Current->Store : 0) &&
              R->Arguments[1] == (PageFault ? Operand : WrappedAddress),
          SiteContext);
  ++ValidatedFaults;
  if (Repair)
    *(U64 *)(C + ContextPointer) = (U64)Memory;
  else
    *(U64 *)(C + ContextPC) = (U64)Current->Resume;
  return ContinueExecution;
}
void entry(void) {
  Memory = VirtualAlloc(0, ReservedPages * PageBytes, MemReserve, PageNoAccess);
  require(Memory && VirtualAlloc(Memory, CommittedPages * PageBytes, MemCommit,
                                 PageReadWrite) == Memory,
          SiteAllocation);
  U32 Previous;
  require(
      VirtualProtect(Memory + PageBytes, PageBytes, PageNoAccess, &Previous),
      SiteProtection);
  void *Handler = AddVectoredExceptionHandler(1, observe);
  require(Handler != 0, SiteHandler);
  const U32 OperationCount = sizeof(Operations) / sizeof(*Operations);
  for (Op = 0; Op < OperationCount; ++Op) {
    Current = &Operations[Op];
    for (U32 I = 0; I < PageBytes / sizeof(U64); ++I)
      ((U64 *)Memory)[I] = VectorHigh ^ I;
    Repair = 0;
    for (Case = 0; Case < ScenarioCount; ++Case) {
      Operand = (U64)Memory + 1;
      U32 Protection = PageReadWrite;
      switch (Case) {
      case ScenarioReadOnly:
        Protection = PageReadOnly;
        break;
      case ScenarioNoAccess:
        Protection = PageNoAccess;
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
        Protection = PageNoAccess;
        break;
      }
      require(VirtualProtect(Memory, PageBytes, Protection, &Previous),
              SiteProtection);
      U64 Vector[2] = {0, 0};
      Calls = 0;
      Current->Run((const void *)Operand, Vector);
      ObservedVector[0] = Vector[0];
      ObservedVector[1] = Vector[1];
      require(Calls == 1, SiteHandler);
      require(Vector[0] == VectorLow && Vector[1] == VectorHigh, SiteVector);
      require(VirtualProtect(Memory, PageBytes, PageReadWrite, &Previous),
              SiteProtection);
      // Preserve every byte for independent host validation without spending
      // the checked execution budget on repeated scalar page comparisons.
      writeOutput(Memory, PageBytes);
      ++CompletedScenarios;
    }
    // Repair the operand in CONTEXT and retry the same faulting instruction.
    for (U32 I = 0; I < VectorBytes; ++I)
      Memory[I] = 0;
    Repair = 1;
    Case = ScenarioReadable;
    Operand = (U64)Memory + 1;
    Calls = 0;
    U64 Vector[2] = {0, 0};
    Current->Run((const void *)Operand, Vector);
    ObservedVector[0] = Vector[0];
    ObservedVector[1] = Vector[1];
    require(Calls == 1, SiteHandler);
    require(Vector[0] == Current->Low && Vector[1] == Current->High,
            SiteVector);
    writeOutput(Memory, PageBytes);
    ++CompletedRetries;
  }
  require(CompletedScenarios == OperationCount * ScenarioCount &&
              CompletedRetries == OperationCount &&
              ValidatedFaults == CompletedScenarios + CompletedRetries,
          SiteOutput);
  require(RemoveVectoredExceptionHandler(Handler) &&
              VirtualFree(Memory, 0, MemRelease),
          SiteCleanup);
  U32 Result[] = {CompletedScenarios, CompletedRetries, ValidatedFaults};
  writeOutput(Result, sizeof(Result));
  ExitProcess(ExitStatus);
}

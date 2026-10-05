//===- aarch64_exclusive_oracle.c - Native Windows ARM64 observations
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "aarch64_exclusive.inc"

#define NEVERD_EXCLUSIVE_ORACLE_VALUE(Name, Value) enum { Name = Value };
#include "AArch64ExclusiveOracle.def"
#undef NEVERD_EXCLUSIVE_ORACLE_VALUE
__declspec(dllimport) void ExitProcess(unsigned);
__declspec(dllimport) void *GetStdHandle(unsigned);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned, unsigned *,
                                    void *);

enum {
#define NEVERD_EXCLUSIVE_ORACLE_FIELD(Name) Field##Name,
#include "AArch64ExclusiveOracle.def"
#undef NEVERD_EXCLUSIVE_ORACLE_FIELD
  FieldCount
};
typedef struct NativeException {
  unsigned Code, Flags;
  struct NativeException *Nested;
  void *Address;
  unsigned Count, Padding;
  ExclusiveWord Parameters[MaxParameters];
} NativeException;
typedef struct {
  NativeException *Record;
  unsigned char *Context;
} NativePointers;
__declspec(dllimport) void *
AddVectoredExceptionHandler(unsigned, unsigned (*)(NativePointers *));
__declspec(dllimport) unsigned RemoveVectoredExceptionHandler(void *);
static ExclusiveWord FaultSite, ResumeSite, Observation[FieldCount];
static unsigned Faults;

static unsigned alignmentHandler(NativePointers *P) {
  ExclusiveWord *PC = (ExclusiveWord *)(P->Context + ContextPCOffset);
  if (*PC != FaultSite || (ExclusiveWord)P->Record->Address != FaultSite ||
      ++Faults != 1 || P->Record->Count > MaxParameters)
    ExitProcess(FailureStatus);
  Observation[FieldCode] = P->Record->Code;
  Observation[FieldFlags] = P->Record->Flags;
  Observation[FieldParameterCount] = P->Record->Count;
  Observation[FieldFaultPC] = FaultSite;
  Observation[FieldExceptionPC] = (ExclusiveWord)P->Record->Address;
  Observation[FieldContextPC] = *PC;
  ExclusiveWord *GPR = (ExclusiveWord *)(P->Context + ContextGPROffset);
  Observation[FieldContextLow] = GPR[0];
  Observation[FieldContextStatus] = GPR[2];
  for (unsigned I = 0; I != P->Record->Count; ++I)
    Observation[FieldParameter0 + I] = P->Record->Parameters[I];
  *PC = ResumeSite;
  return ContinueExecution;
}

static void emit(const void *Data, unsigned Size) {
  unsigned Written = 0;
  if (!WriteFile(GetStdHandle(StdoutSelector), Data, Size, &Written, 0) ||
      Written != Size)
    ExitProcess(FailureStatus);
}

#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  static void alignment##Name(unsigned Index) {                                \
    if (Width == 1)                                                            \
      return;                                                                  \
    ExclusivePair Memory = {ExclusiveInitial, ExclusiveUpdated};               \
    for (unsigned I = 0; I != FieldCount; ++I)                                 \
      Observation[I] = 0;                                                      \
    Observation[FieldCaseIndex] = Index;                                       \
    Faults = 0;                                                                \
    ExclusiveWord Low, Status;                                                 \
    __asm__ volatile("adr x4, 1f\n\tstr x4, [%[fault]]\n\t"                    \
                     "adr x4, 2f\n\tstr x4, [%[resume]]\n\t"                   \
                     "mov x1, %[address]\n\tmov x0, %[low_seed]\n\t"           \
                     "mov x2, %[status_seed]\n\t1:\n\t.inst " #Load            \
                     "\n\t2:\n\t"                                              \
                     "mov %[low], x0\n\tmov %[status], x2\n\t"                 \
                     : [low] "=&r"(Low), [status] "=&r"(Status)                \
                     : [fault] "r"(&FaultSite), [resume] "r"(&ResumeSite),     \
                       [address] "r"((unsigned char *)&Memory + 1),            \
                       [low_seed] "r"(ExclusiveInitial),                       \
                       [status_seed] "r"(ExclusiveUpdated)                     \
                     : "x0", "x1", "x2", "x3", "x4", "memory");                \
    if (Faults != 1 || Low != ExclusiveInitial || Status != ExclusiveUpdated)  \
      ExitProcess(FailureStatus);                                              \
    emit(Observation, sizeof(Observation));                                    \
  }
#include "../AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE

void entry(void) {
  unsigned Results[] = {
#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  checkExclusive##Name(),
#include "../AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE
  };
  emit(Results, sizeof(Results));
  for (unsigned I = 0; I != sizeof(Results) / sizeof(*Results); ++I)
    if (!Results[I])
      ExitProcess(FailureStatus);
  void *Handler = AddVectoredExceptionHandler(1, alignmentHandler);
  if (!Handler)
    ExitProcess(FailureStatus);
  unsigned Index = 0;
#define NEVERD_EXCLUSIVE_CASE(Name, Load, Store, Width, Count)                 \
  alignment##Name(Index++);
#include "../AArch64ExclusiveCases.def"
#undef NEVERD_EXCLUSIVE_CASE
  if (!RemoveVectoredExceptionHandler(Handler))
    ExitProcess(FailureStatus);
  ExitProcess(0);
}

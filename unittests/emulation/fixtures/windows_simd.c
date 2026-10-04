//===- windows_simd.c - Independent Windows SIMD exception observations --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include <windows.h>

typedef unsigned long long U64;
#define NEVERD_WINDOWS_SIMD_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_WINDOWS_SIMD_WIDE(Name, Value) static const U64 Name = Value;
#include "WindowsSIMDCases.def"
#undef NEVERD_WINDOWS_SIMD_WIDE
#undef NEVERD_WINDOWS_SIMD_VALUE
enum Field {
#define NEVERD_WINDOWS_SIMD_FIELD(Name) Field##Name,
#include "WindowsSIMDCases.def"
#undef NEVERD_WINDOWS_SIMD_FIELD
  FieldCount
};
extern void SaveInitial(void *);
#define NEVERD_WINDOWS_SIMD_LEAF(Name)                                         \
  extern void Run##Name(const void *, void *, void *, const void *);           \
  extern void Fault##Name(void), Resume##Name(void);
#define NEVERD_SIMD_CASE(Name, ...)                                            \
  NEVERD_WINDOWS_SIMD_LEAF(Register##Name)                                     \
  NEVERD_WINDOWS_SIMD_LEAF(Memory##Name)
#include "../X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_CASE
#undef NEVERD_WINDOWS_SIMD_LEAF

typedef struct {
  void (*Run)(const void *, void *, void *, const void *);
  void (*Fault)(void);
  void (*Resume)(void);
} Leaf;
typedef struct {
  unsigned Status, Left, Right, MaskedStatus, MaskedResult;
  Leaf Forms[2];
} Operation;
static const Operation Operations[] = {
#define NEVERD_WINDOWS_SIMD_LEAF(Name) {Run##Name, Fault##Name, Resume##Name}
#define NEVERD_SIMD_CASE(Name, Status, Left, Right, FaultStatus, MaskedStatus, \
                         MaskedResult, ...)                                    \
  {Status,                                                                     \
   Left,                                                                       \
   Right,                                                                      \
   MaskedStatus,                                                               \
   MaskedResult,                                                               \
   {NEVERD_WINDOWS_SIMD_LEAF(Register##Name),                                  \
    NEVERD_WINDOWS_SIMD_LEAF(Memory##Name)}},
#include "../X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_CASE
#undef NEVERD_WINDOWS_SIMD_LEAF
};
static const unsigned StatusCodes[] = {
#define NEVERD_WINDOWS_SIMD_STATUS(Active, MXCSR, Code) [Active] = Code,
#include "WindowsSIMDStatusCases.def"
#undef NEVERD_WINDOWS_SIMD_STATUS
};
_Alignas(VectorBytes) static unsigned char Initial[StateBytes],
    Before[StateBytes], After[StateBytes], Host[StateBytes];
static U64 Record[FieldCount];
static const Leaf *Current;

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
  require(!Record[FieldTraps]++ &&
              (U64)E->ExceptionAddress == (U64)Current->Fault &&
              C->Rip == (U64)Current->Fault && !E->ExceptionRecord &&
              E->NumberParameters <= EXCEPTION_MAXIMUM_PARAMETERS,
          SiteContext);
  Record[FieldCode] = E->ExceptionCode;
  Record[FieldFlags] = E->ExceptionFlags;
  Record[FieldParameterCount] = E->NumberParameters;
  for (unsigned I = 0; I < E->NumberParameters; ++I)
    Record[FieldParameter0 + I] = E->ExceptionInformation[I];
  Record[FieldExceptionPC] = (U64)E->ExceptionAddress;
  Record[FieldContextPC] = C->Rip;
  Record[FieldContextFlags] = C->ContextFlags;
  Record[FieldContextMXCSR] = C->MxCsr;
  Record[FieldContextFXMXCSR] = C->FltSave.MxCsr;
  const unsigned char *Vectors = (const unsigned char *)C->FltSave.XmmRegisters;
  for (unsigned I = 0; I < VectorBytes * VectorCount; ++I)
    require(Vectors[I] == Before[XmmOffset + I], SiteVector);
  // Literal expectations were frozen from the independent original Windows
  // run before production classification was implemented.
  const unsigned Active = C->MxCsr & ~(C->MxCsr >> MaskShift) & Sticky;
  require(Active && E->ExceptionCode == StatusCodes[Active] &&
              !E->ExceptionFlags &&
              E->NumberParameters == ExceptionParameterCount &&
              !E->ExceptionInformation[0] &&
              E->ExceptionInformation[1] == C->MxCsr &&
              C->MxCsr == C->FltSave.MxCsr,
          SiteStatus);
  C->Rip = (U64)Current->Resume;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void entry(void) {
  SYSTEM_INFO Info;
  GetNativeSystemInfo(&Info);
  require(Info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64,
          SiteHost);
  SaveInitial(Initial);
  void *Handler = AddVectoredExceptionHandler(1, observe);
  require(Handler != 0, SiteHandler);
  for (unsigned Op = 0; Op < sizeof(Operations) / sizeof(*Operations); ++Op)
    for (unsigned Memory = 0; Memory < 2; ++Memory) {
      const Operation *O = Operations + Op;
      Current = O->Forms + Memory;
      for (unsigned Unmask = 0; Unmask < MaskCount; ++Unmask)
        for (unsigned HasSticky = 0; HasSticky < 2; ++HasSticky) {
          for (unsigned I = 0; I < FieldCount; ++I)
            Record[I] = 0;
          Record[FieldOperation] = Op;
          Record[FieldMemory] = Memory;
          Record[FieldUnmask] = Unmask;
          Record[FieldSticky] = HasSticky;
          Record[FieldFaultPC] = (U64)Current->Fault;
          for (unsigned I = 0; I < StateBytes; ++I)
            Before[I] = Initial[I];
          const unsigned Control = (DefaultMXCSR & ~(Unmask << MaskShift)) |
                                   (HasSticky ? Sticky : 0);
          *(unsigned *)(Before + MXCSROffset) = Control;
          Record[FieldBeforeMXCSR] = Control;
          for (unsigned I = 0; I < VectorBytes * VectorCount / sizeof(U64); ++I)
            ((U64 *)(Before + XmmOffset))[I] = Sentinel + I;
          *(unsigned *)(Before + XmmOffset) = O->Left;
          *(unsigned *)(Before + XmmOffset + VectorBytes) = O->Right;
          const U64 Operand[] = {O->Right, Sentinel};
          Current->Run(Before, After, Host, Operand);
          Record[FieldAfterMXCSR] = *(unsigned *)(After + MXCSROffset);
          Record[FieldAfterValue] = *(U64 *)(After + XmmOffset);
          require(Record[FieldTraps] <= 1 && (Unmask || !Record[FieldTraps]) &&
                      (!(Unmask & O->Status) || Record[FieldTraps]),
                  SiteTrap);
          for (unsigned I = Record[FieldTraps] ? 0 : sizeof(unsigned);
               I < VectorBytes * VectorCount; ++I)
            require(After[XmmOffset + I] == Before[XmmOffset + I], SiteVector);
          if (!Unmask)
            require(Record[FieldAfterMXCSR] == (Control | O->MaskedStatus) &&
                        *(unsigned *)(After + XmmOffset) == O->MaskedResult,
                    SiteMasked);
          DWORD Written;
          require(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Record,
                            sizeof(Record), &Written, 0) &&
                      Written == sizeof(Record),
                  SiteOutput);
        }
    }
  require(RemoveVectoredExceptionHandler(Handler), SiteCleanup);
  ExitProcess(0);
}

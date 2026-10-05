//===- driver_seh_simd.inc - Original WDK SSE fault/filter/continuation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
C_ASSERT(sizeof(CONTEXT) == SehSIMDContextSize);
C_ASSERT(FIELD_OFFSET(CONTEXT, MxCsr) == SehSIMDContextMXCSR);
C_ASSERT(FIELD_OFFSET(CONTEXT, FltSave) == SehSIMDContextFX);
C_ASSERT(FIELD_OFFSET(CONTEXT, FltSave.MxCsr) == SehSIMDContextFXMXCSR);
C_ASSERT(FIELD_OFFSET(CONTEXT, Xmm0) == SehSIMDContextXmm);
C_ASSERT(INITIAL_MXCSR == SehSIMDDefault);

typedef struct {
  ULONG Operation, Bit, Left, Right, Status, MaskedStatus, MaskedResult, Code;
} SIMD_CASE;
static const SIMD_CASE SimdCases[] = {
#define NEVERD_SEH_SIMD_CASE(Name, ...) {__VA_ARGS__},
#include "driver_seh_simd.def"
#undef NEVERD_SEH_SIMD_CASE
};
static const SIMD_CASE *SimdCase;
static volatile BOOLEAN SimdValid;
static ULONG64 SimdResult[SehSIMDRegisterWords];
static ULONG64 SimdVolatile[SehSIMDRegisterWords];
static ULONG64 SimdNonvolatile[SehSIMDRegisterWords];
static ULONG SimdAfter;
static ULONG64 SimdFlags;

__declspec(noinline) static VOID SimdInstruction(VOID) {
  const ULONG64 Seed[SehSIMDRegisterWords] = {SehSIMDSeed, SehSIMDSeed};
  const ULONG64 Left[SehSIMDRegisterWords] = {SehSIMDHighLane | SimdCase->Left,
                                              SehSIMDSeed};
  const ULONG64 Right[SehSIMDRegisterWords] = {
      SehSIMDHighLane | SimdCase->Right, SehSIMDSeed};
  const ULONG Control = SehSIMDDefault & ~(SimdCase->Bit << SehSIMDMaskShift);
  const ULONG Default = SehSIMDDefault;
  switch (SimdCase->Operation) {
  case SehSIMDDivide:
    NEVERD_SEH_SIMD_EXECUTE(NEVERD_SEH_SIMD_DIVIDE);
    break;
  case SehSIMDAdd:
    NEVERD_SEH_SIMD_EXECUTE(NEVERD_SEH_SIMD_ADD);
    break;
  case SehSIMDMultiply:
    NEVERD_SEH_SIMD_EXECUTE(NEVERD_SEH_SIMD_MULTIPLY);
    break;
  }
}

static LONG SimdFilter(PEXCEPTION_POINTERS Pointers) {
  const PCONTEXT Context = Pointers->ContextRecord;
  const PEXCEPTION_RECORD Record = Pointers->ExceptionRecord;
  const ULONG Expected =
      (SehSIMDDefault & ~(SimdCase->Bit << SehSIMDMaskShift)) |
      SimdCase->Status;
  ULONG LiveMXCSR = 0;
  ULONG64 LiveFlags = 0;
  __asm__ volatile(NEVERD_SEH_SIMD_LIVE : "=m"(LiveMXCSR), "=r"(LiveFlags));
  SimdValid =
      Stage == SehSIMDBefore && LiveMXCSR == SehSIMDDefault &&
      !(LiveFlags & SehSIMDDirection) &&
      Record->ExceptionCode == SimdCase->Code && !Record->ExceptionFlags &&
      Record->NumberParameters == SehSIMDParameterCount &&
      Record->ExceptionAddress == (PVOID)(ULONG_PTR)Context->Rip &&
      !Record->ExceptionInformation[0] &&
      Record->ExceptionInformation[1] == Expected &&
      (Context->ContextFlags & CONTEXT_FLOATING_POINT) ==
          CONTEXT_FLOATING_POINT &&
      Context->MxCsr == Expected && Context->FltSave.MxCsr == Expected &&
      (Context->EFlags & SehSIMDDirection) && !(Context->EFlags & SehSIMDTrap);
  for (ULONG I = 0; I < SehSIMDRegisterCount; ++I) {
    const PM128A Register = &Context->Xmm0 + I;
    const ULONG64 Low = I == 0   ? SehSIMDHighLane | SimdCase->Left
                        : I == 1 ? SehSIMDHighLane | SimdCase->Right
                                 : SehSIMDSeed;
    SimdValid = SimdValid && Register->Low == Low &&
                (ULONG64)Register->High == SehSIMDSeed;
  }
  DbgPrint(SehSIMDFilterMessage, Record->ExceptionCode, Mode);
  if (Mode == SehSIMDRejectX87) {
    Context->FltSave.ControlWord ^= 1;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (SimdValid && (Mode == SehSIMDMaskRetry || Mode == SehSIMDOperandRetry)) {
    if (Mode == SehSIMDMaskRetry) {
      Context->MxCsr = SehSIMDDefault | SehSIMDReserved;
    } else {
      Context->MxCsr &= ~SehSIMDSticky;
      Context->Xmm0.Low = SehSIMDHighLane | SehSIMDOne;
      Context->Xmm1.Low = SehSIMDHighLane | SehSIMDOne;
    }
    // The kernel consumes the top-level field; this contradictory copy and
    // high reserved bits must not control the resumed instruction.
    Context->FltSave.MxCsr = SehSIMDIgnoredFX;
    __asm__ volatile(NEVERD_SEH_SIMD_CLOBBER : : : "xmm0", "xmm1", "xmm5");
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) static VOID SimdWithFinally(VOID) {
  __try {
    SimdInstruction();
  } __finally {
    ULONG LiveMXCSR = 0;
    ULONG64 LiveFlags = 0;
    __asm__ volatile(NEVERD_SEH_SIMD_LIVE : "=m"(LiveMXCSR), "=r"(LiveFlags));
    SimdValid = SimdValid && LiveMXCSR == SehSIMDDefault &&
                !(LiveFlags & SehSIMDDirection) &&
                (AbnormalTermination() == (Mode == SehSIMDHandle));
    Stage = SehSIMDFinally;
    DbgPrint(SehSIMDFinallyMessage, AbnormalTermination());
  }
}

__declspec(noinline) static NTSTATUS ProcessorSIMD(VOID) {
  for (ULONG I = 0; I < RTL_NUMBER_OF(SimdCases); ++I) {
    SimdCase = &SimdCases[I];
    SimdValid = Mode == SehSIMDConstant;
    Stage = SehSIMDBefore;
    if (Mode == SehSIMDConstant) {
      __try {
        SimdInstruction();
        SimdValid = FALSE;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        REQUIRE((ULONG)GetExceptionCode() == SimdCase->Code);
        Stage = SehSIMDHandler;
      }
    } else {
      __try {
        SimdWithFinally();
      } __except (SimdFilter(GetExceptionInformation())) {
        REQUIRE((ULONG)GetExceptionCode() == SimdCase->Code);
        REQUIRE(Stage == SehSIMDFinally);
        REQUIRE(Mode == SehSIMDHandle);
        Stage = SehSIMDHandler;
      }
    }
    REQUIRE(SimdValid);
    if (Mode == SehSIMDMaskRetry || Mode == SehSIMDOperandRetry) {
      const ULONG Result = Mode == SehSIMDMaskRetry ? SimdCase->MaskedResult
                           : SimdCase->Operation == SehSIMDAdd ? SehSIMDTwo
                                                               : SehSIMDOne;
      const ULONG MXCSR =
          Mode == SehSIMDMaskRetry
              ? SehSIMDDefault | SimdCase->MaskedStatus
              : SehSIMDDefault & ~(SimdCase->Bit << SehSIMDMaskShift);
      REQUIRE(Stage == SehSIMDFinally && (SimdFlags & SehSIMDDirection));
      REQUIRE(SimdAfter == MXCSR);
      REQUIRE(SimdResult[0] == (SehSIMDHighLane | Result));
      REQUIRE(SimdResult[1] == SehSIMDSeed);
      for (ULONG Word = 0; Word < SehSIMDRegisterWords; ++Word) {
        REQUIRE(SimdVolatile[Word] == SehSIMDSeed);
        REQUIRE(SimdNonvolatile[Word] == SehSIMDSeed);
      }
    } else {
      ULONG LiveMXCSR = 0;
      ULONG64 LiveFlags = 0;
      __asm__ volatile(NEVERD_SEH_SIMD_LIVE : "=m"(LiveMXCSR), "=r"(LiveFlags));
      REQUIRE(Stage == SehSIMDHandler && LiveMXCSR == SehSIMDDefault);
      REQUIRE(!(LiveFlags & SehSIMDDirection));
    }
    DbgPrint(SehSIMDCompleteMessage, I);
  }
  return STATUS_SUCCESS;
}

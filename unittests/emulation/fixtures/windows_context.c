//===- windows_context.c - Original Windows caller-context oracle --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef unsigned short U16;
typedef unsigned char U8;
#define NEVERD_CAPTURE_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_CAPTURE_WIDE(Name, Value) static const U64 Name = Value;
#define NEVERD_CAPTURE_TEXT(Name, Text) static const char Name[] = Text;
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_TEXT
#undef NEVERD_CAPTURE_VALUE
#undef NEVERD_CAPTURE_WIDE
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) U16 *GetCommandLineW(void);
__declspec(dllimport) void SetLastError(U32);
__declspec(dllimport) U32 GetLastError(void);
__declspec(dllimport) void *GetModuleHandleA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) void RtlCaptureContext(void *);
__declspec(dllimport) void *AddVectoredExceptionHandler(U32, U32 (*)(void *));
__declspec(dllimport) U32 RemoveVectoredExceptionHandler(void *);
__declspec(dllimport) void RaiseException(U32, U32, U32, const U64 *);
typedef void (*Capture)(void *);
extern void CaptureFixture(Capture, void *, U64 *);
extern void CaptureResume(void);
__declspec(align(16)) const U64 VectorSeeds[] = {
#define NEVERD_CAPTURE_VECTOR(Index, Low, High) (U64)(Low), (U64)(High),
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_VECTOR
};
#if defined(__x86_64__)
enum { Size = X64Size };
#ifdef NEVERD_CAPTURE_FP_PROBE
__declspec(align(16)) U8 SeededFPState[X64FXSize];
__declspec(align(16)) const U64 FPSeed[X64FXSize / sizeof(U64)] = {
#define NEVERD_CAPTURE_FP_WORD(Index, Value) [Index] = Value,
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_FP_WORD
};
#endif
#else
enum { Size = ARM64Size };
#endif
__declspec(align(16)) static U8 Context[Size];
static U64 Observed[ObservationWords];
static U32 Mode, CallbackCount;
static void require(int Valid, U32 Site) {
  if (Valid)
    return;
  U64 Failure[] = {Site, Mode};
  U32 Written;
  WriteFile(GetStdHandle(StderrSelector), Failure, sizeof(Failure), &Written,
            0);
  ExitProcess(FailureStatus);
}
static void emit(const void *Data, U32 Count) {
  U32 Written;
  require(WriteFile(GetStdHandle(StdoutSelector), Data, Count, &Written, 0) &&
              Written == Count,
          SiteWrite);
}
static void run(Capture API, U8 Fill) {
  for (U32 I = 0; I < Size; ++I)
    Context[I] = Fill;
  SetLastError(LastErrorSeed);
  CaptureFixture(API, Context, Observed);
  require(GetLastError() == LastErrorSeed, SiteLastError);
  if (Mode == ProbeMode) {
    emit(Context, sizeof(Context));
    emit(Observed, sizeof(Observed));
    return;
  }
#if defined(__x86_64__)
  require(*(U32 *)(Context + X64FlagsOffset) == X64Flags, SiteFlags);
  require(*(U64 *)(Context + X64SP) == Observed[StackIndex], SiteSP);
  require(*(U64 *)(Context + X64PC) == Observed[PCIndex], SitePC);
#define NEVERD_CAPTURE_X64_GPR(Name, Index, Value)                             \
  require(*(U64 *)(Context + X64GPR + Index * sizeof(U64)) == (Value), SiteGPR);
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_X64_GPR
  require(*(U64 *)(Context + X64GPR + sizeof(U64)) == (U64)Context, SiteGPR);
  require((*(U32 *)(Context + X64StatusOffset) & X64FlagMask) ==
              (Observed[FlagsIndex] & X64FlagMask),
          SiteFlagsValue);
  require(*(U32 *)(Context + X64ControlOffset) ==
              *(U32 *)(Context + X64FPOffset + X64MXCSRField),
          SiteControl);
#ifdef NEVERD_CAPTURE_FP_PROBE
  require(*(U32 *)(Context + X64ControlOffset) == X64Control, SiteControl);
  require(*(U64 *)(SeededFPState + X64FPIPField) == X64FPIP &&
              *(U64 *)(SeededFPState + X64FPDPField) == X64FPDP,
          SiteControl);
  require(*(U16 *)(Context + X64FPOffset) == X64FPControl &&
              *(U16 *)(Context + X64FPOffset + X64FPStatusField) ==
                  X64FPStatus &&
              Context[X64FPOffset + X64FPTagField] == X64FPTag &&
              *(U16 *)(Context + X64FPOffset + X64FPOpcodeField) == X64FPOpcode,
          SiteControl);
  require(*(U64 *)(Context + X64FPOffset + X64FPIPField) == (U32)X64FPIP &&
              *(U64 *)(Context + X64FPOffset + X64FPDPField) == (U32)X64FPDP,
          SiteControl);
  for (U32 I = 0; I < X64FPRegisterCount; ++I) {
    const U8 *Register = Context + X64FPRegistersOffset + I * VectorBytes;
    require(*(U64 *)Register == (U64)X64FPMantissa + I &&
                *(U16 *)(Register + sizeof(U64)) == X64FPExponent,
            SiteVector);
  }
#endif
  for (U32 I = 0; I < X64FlagsOffset; ++I)
    require(Context[I] == Fill, SitePreserve);
  for (U32 I = X64StatusOffset + sizeof(U32); I < X64GPR; ++I)
    require(Context[I] == Fill, SitePreserve);
  for (U32 I = X64TailOffset; I < Size; ++I)
    require(Context[I] == Fill, SitePreserve);
  for (U32 I = 0; I < X64VectorCount * VectorBytes; ++I)
    require(Context[X64Vector + I] == ((const U8 *)VectorSeeds)[I], SiteVector);
#else
  require(*(U32 *)Context == ARM64Flags, SiteFlags);
  require(*(U64 *)(Context + ARM64SP) == Observed[StackIndex], SiteSP);
  require(*(U64 *)(Context + ARM64PC) == Observed[PCIndex], SitePC);
  require(*(U64 *)(Context + ARM64LR) == 0, SitePC);
  require(*(U64 *)(Context + ARM64X0Offset) == 0, SiteGPR);
  require(*(U64 *)(Context + ARM64X18Offset) == Observed[TEBIndex], SiteGPR);
#define NEVERD_CAPTURE_ARM64_GPR(Name, Index, Value)                           \
  require(*(U64 *)(Context + ARM64GPR + Index * sizeof(U64)) == (Value),       \
          SiteGPR);
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_ARM64_GPR
  require(*(U32 *)(Context + ARM64StatusOffset) == ARM64FlagSeed,
          SiteFlagsValue);
  require(*(U32 *)(Context + ARM64ControlOffset) == ARM64Control &&
              *(U32 *)(Context + ARM64FPSROffset) == ARM64Status,
          SiteControl);
  for (U32 I = ARM64TailOffset; I < Size; ++I)
    require(Context[I] == Fill, SitePreserve);
  for (U32 I = 0; I < ARM64VectorCount * VectorBytes; ++I)
    require(Context[ARM64Vector + I] == ((const U8 *)VectorSeeds)[I],
            SiteVector);
#endif
}
static U32 handler(void *Exception) {
  ++CallbackCount;
  run(RtlCaptureContext, Sentinel);
  run(RtlCaptureContext, (U8)~Sentinel);
  return ContinueExecution;
}
void entry(void) {
  const U16 *Line = GetCommandLineW();
  Mode = DirectMode;
  for (U32 I = 0; Line[I]; ++I)
    if (Line[I] == ModePrefix && Line[I + 1])
      Mode = Line[I + 1];
  Capture API = RtlCaptureContext;
  if (Mode == NativeMode || Mode == BaseMode) {
    API = (Capture)GetProcAddress(
        GetModuleHandleA(Mode == NativeMode ? Native : Base), Symbol);
    require(API != 0, SiteProvider);
  }
  if (Mode == CallbackMode) {
    void *Handle = AddVectoredExceptionHandler(1, handler);
    require(Handle != 0, SiteHandler);
    RaiseException(SoftwareCode, 0, 0, 0);
    require(CallbackCount == 1 && RemoveVectoredExceptionHandler(Handle),
            SiteHandler);
  } else {
    run(API, Sentinel);
    run(API, (U8)~Sentinel);
  }
  if (Mode != ProbeMode)
    emit(&Mode, sizeof(Mode));
  ExitProcess(CompletionStatus);
}

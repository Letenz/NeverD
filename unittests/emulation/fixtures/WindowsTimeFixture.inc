//===- WindowsTimeFixture.inc - Independent PE clock and delay checks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
__declspec(dllimport) int QueryPerformanceCounter(long long *Value);
__declspec(dllimport) int QueryPerformanceFrequency(long long *Value);
__declspec(dllimport) DWORD GetTickCount(void);
__declspec(dllimport) void GetSystemTimeAsFileTime(ULONG_PTR *Value);
__declspec(dllimport) long ZwDelayExecution(unsigned char Alertable,
                                            const long long *Interval);

// Only the low BOOLEAN byte is defined. Preserve the native ABI while making
// undefined high register bits visible to the model on both guest ISAs.
__attribute__((naked)) static long delayLowByte(unsigned char Alertable,
                                                const long long *Interval) {
#define NEVERD_WINDOWS_CLOCK_ASM(Text) __asm__(Text);
#include "WindowsProcessCases.def"
#undef NEVERD_WINDOWS_CLOCK_ASM
}

static void clockScenario(char Mode) {
  if (Mode != ClockServices[0] && Mode != ClockAlertable[0] &&
      Mode != ClockAbsolute[0] && Mode != ClockTooLong[0])
    return;
  long long Interval = -ClockDelayTicks;
  if (Mode == ClockAbsolute[0])
    Interval = ClockFutureInterval;
  else if (Mode == ClockTooLong[0])
    Interval = (long long)ClockMinimumInterval;
  if (Mode != ClockServices[0]) {
    const long Status = delayLowByte(Mode == ClockAlertable[0], &Interval);
    const ULONG_PTR Observed[] = {(ULONG_PTR)Interval, (DWORD)Status};
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), Observed, sizeof(Observed),
              &Written, 0);
    require(0);
    return;
  }
  long long Frequency = 0, BeforeCounter = 0, AfterCounter = 0;
  ULONG_PTR BeforeFileTime = 0, AfterFileTime = 0;
  SetLastError(LastErrorSeed);
  require(QueryPerformanceFrequency(&Frequency) && Frequency > 0);
  require(QueryPerformanceCounter(&BeforeCounter));
  GetSystemTimeAsFileTime(&BeforeFileTime);
  const DWORD BeforeTick = GetTickCount();
  require(delayLowByte(0, &Interval) == 0);
  const DWORD AfterTick = GetTickCount();
  GetSystemTimeAsFileTime(&AfterFileTime);
  require(QueryPerformanceCounter(&AfterCounter));
  const DWORD Error = GetLastError();
  // Millisecond wrap is unsigned; allow host descheduling without a tight
  // timing assertion. The wall clock has no monotonicity requirement.
  const int OK =
      BeforeCounter > 0 && AfterCounter > BeforeCounter &&
      AfterCounter - BeforeCounter >= Frequency / ClockMillisecondsPerSecond &&
      BeforeFileTime > ClockFileTimeEpoch &&
      AfterFileTime > ClockFileTimeEpoch &&
      (DWORD)(AfterTick - BeforeTick) < ClockTickWindow &&
      Error == LastErrorSeed;
  if (!OK) {
    const ULONG_PTR Observed[] = {(ULONG_PTR)Frequency,
                                  (ULONG_PTR)BeforeCounter,
                                  (ULONG_PTR)AfterCounter,
                                  BeforeFileTime,
                                  AfterFileTime,
                                  BeforeTick,
                                  AfterTick,
                                  Error};
    DWORD Written;
    WriteFile(GetStdHandle(StderrSelector), Observed, sizeof(Observed),
              &Written, 0);
  }
  require(OK);
}

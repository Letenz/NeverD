//===- nonlocal_finally_runtime.c - MSVC nonlocal finally oracle ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original compiler/runtime observations for ordinary and unwinding finally
/// invocations. These source images do not certify NeverD reconstruction.

#include <intrin.h>
#include <stdio.h>
#include <windows.h>

#define PROBE_EXCEPTION 0xE0424242u
#define RESUME_EXCEPTION 0xE0424243u
#ifndef FINALLY_NEGATIVE_CONTROL
#define FINALLY_NEGATIVE_CONTROL 0
#endif
#define INNER_MARK (FINALLY_NEGATIVE_CONTROL ? 8u : 3u)

static volatile unsigned ImmediateGuardsOK = 1, ImmediateChainOK = 1;

__declspec(dllexport) __declspec(noinline) unsigned
nonlocal_finally_goto(unsigned Leave, unsigned Throwing) {
  volatile unsigned Value = 0;
  __try {
    __try {
      __try {
        Value = Value * 10 + 1;
        if (Throwing)
          RaiseException(PROBE_EXCEPTION, 0, 0, NULL);
        Value = Value * 10 + 2;
      } __finally {
        Value = Value * 10 + INNER_MARK;
        if (Leave)
          goto Resume;
      }
      Value = Value * 10 + 4;
    } __finally {
      Value = Value * 10 + 5;
    }
  } __except (GetExceptionCode() == PROBE_EXCEPTION
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    Value = Value * 10 + 6;
  }
Resume:
  Value = Value * 10 + 7;
  return Value;
}

__declspec(dllexport) __declspec(noinline) unsigned
nonlocal_finally_return(unsigned Throwing) {
  volatile unsigned Value = 1;
  __try {
    if (Throwing)
      RaiseException(PROBE_EXCEPTION, 0, 0, NULL);
    Value = Value * 10 + 2;
  } __finally {
    Value = Value * 10 + INNER_MARK;
    return Value;
  }
}

// Keep the resumed path independent of stdio and other opaque helpers. A new
// exception must search past the inner filter to the still-live outer handler;
// abandoned finally/runtime guards must already have been retired.
__declspec(dllexport) __declspec(noinline) unsigned
nonlocal_finally_reenter(unsigned Leave, unsigned Throwing) {
  volatile unsigned Trace = 1;
  volatile unsigned Guards[2] = {0x51A7E001u, 0x51A7E002u};
#if defined(_M_IX86)
  const unsigned Chain = __readfsdword(0);
#endif
  __try {
    __try {
      __try {
        if (Throwing)
          RaiseException(PROBE_EXCEPTION, 0, 0, NULL);
      } __finally {
        Trace = Trace * 10 + 2;
        // Unlike the legacy compiler-observation cases above, this guard oracle
        // must finish the termination handler during exception unwinding.
        // C4532 makes an abnormal jump out of __finally undefined; an abandoned
        // runtime guard can prevent the second exception from finding handlers.
        if (Leave && !AbnormalTermination())
          goto Resume;
      }
      Trace = Trace * 10 + 3;
    } __except (GetExceptionCode() == PROBE_EXCEPTION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      Trace = Trace * 10 + 4;
    }
  Resume:
    ImmediateGuardsOK &= Guards[0] == 0x51A7E001u && Guards[1] == 0x51A7E002u;
#if defined(_M_IX86)
    ImmediateChainOK &= Chain == __readfsdword(0);
#endif
    __try {
      RaiseException(RESUME_EXCEPTION, 0, 0, NULL);
    } __except (GetExceptionCode() == RESUME_EXCEPTION
                    ? (Trace = Trace * 10 + 5, EXCEPTION_CONTINUE_SEARCH)
                    : EXCEPTION_CONTINUE_SEARCH) {
      // The inner filter must not handle the second exception.
      Trace = Trace * 10 + 8;
    }
  } __except (GetExceptionCode() == RESUME_EXCEPTION
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    Trace = Trace * 10 + 6;
  }
  ImmediateGuardsOK &= Guards[0] == 0x51A7E001u && Guards[1] == 0x51A7E002u;
  return Trace;
}

// This separate helper exposes a scalar input, native exception-field reads
// and a write through a caller-owned pointer. Its returning-stack proof cannot
// stand in for the conditional object-access proof.
__declspec(dllexport) __declspec(noinline) unsigned
nonlocal_finally_counter(unsigned Throwing, volatile unsigned *Counter) {
  __try {
    __try {
      if (Throwing)
        RaiseException(PROBE_EXCEPTION, 0, 0, NULL);
    } __except (GetExceptionCode() == PROBE_EXCEPTION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
      ++*Counter;
    }
  } __finally {
    *Counter += 10;
  }
  return *Counter;
}

int main(void) {
  volatile unsigned Guards[16];
  unsigned Values[6] = {0};
  unsigned FirstValues[6] = {0};
  unsigned GuardsOK = 1, ChainOK = 1, Consistent = 1, Iterations = 0;
  unsigned Reentry[4] = {0}, FirstReentry[4] = {0};
  unsigned Counters[2] = {0}, FirstCounters[2] = {0};
  volatile unsigned CounterGuard[3];
  unsigned Reentries = 0;
  unsigned Round, I;
#if defined(_M_IX86)
  const unsigned Chain = __readfsdword(0);
#endif
  for (Round = 0; Round != 32; ++Round) {
    for (I = 0; I != 16; ++I)
      Guards[I] = 0xC0DE0000u ^ (Round * 16 + I);
    Values[0] = nonlocal_finally_goto(0, 0);
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
    Values[1] = nonlocal_finally_goto(1, 0);
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
    Values[2] = nonlocal_finally_goto(0, 1);
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
    Values[3] = nonlocal_finally_goto(1, 1);
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
    Values[4] = nonlocal_finally_return(0);
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
    Values[5] = nonlocal_finally_return(1);
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
    for (I = 0; I != 6; ++I)
      if (Round == 0)
        FirstValues[I] = Values[I];
      else
        Consistent &= FirstValues[I] == Values[I];
    Iterations += 6;
    for (I = 0; I != 4; ++I) {
      Reentry[I] = nonlocal_finally_reenter(I & 1, I >> 1);
      if (Round == 0)
        FirstReentry[I] = Reentry[I];
      else
        Consistent &= FirstReentry[I] == Reentry[I];
#if defined(_M_IX86)
      ChainOK &= Chain == __readfsdword(0);
#endif
    }
    for (I = 0; I != 2; ++I) {
      CounterGuard[0] = 0xC0110001u;
      CounterGuard[1] = Round;
      CounterGuard[2] = 0xC0110002u;
      Counters[I] = nonlocal_finally_counter(I, &CounterGuard[1]) - Round;
      GuardsOK &= CounterGuard[0] == 0xC0110001u &&
                  CounterGuard[1] == Round + 10 + I &&
                  CounterGuard[2] == 0xC0110002u;
      if (Round == 0)
        FirstCounters[I] = Counters[I];
      else
        Consistent &= FirstCounters[I] == Counters[I];
#if defined(_M_IX86)
      ChainOK &= Chain == __readfsdword(0);
#endif
    }
    Reentries += 6;
    for (I = 0; I != 16; ++I)
      GuardsOK &= Guards[I] == (0xC0DE0000u ^ (Round * 16 + I));
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
  }
  printf("NONLOCAL_FINALLY %u %u %u %u %u %u guards=%u chain=%u "
         "consistent=%u iterations=%u reenter=%u,%u,%u,%u counters=%u,%u "
         "immediate_guards=%u immediate_chain=%u reentries=%u\n",
         Values[0], Values[1], Values[2], Values[3], Values[4], Values[5],
         GuardsOK, ChainOK, Consistent, Iterations, Reentry[0], Reentry[1],
         Reentry[2], Reentry[3], Counters[0], Counters[1], ImmediateGuardsOK,
         ImmediateChainOK, Reentries);
  return !(GuardsOK && ChainOK && Consistent && ImmediateGuardsOK &&
           ImmediateChainOK);
}

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
#ifndef FINALLY_NEGATIVE_CONTROL
#define FINALLY_NEGATIVE_CONTROL 0
#endif
#define INNER_MARK (FINALLY_NEGATIVE_CONTROL ? 8u : 3u)

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

int main(void) {
  volatile unsigned Guards[16];
  unsigned Values[6] = {0};
  unsigned FirstValues[6] = {0};
  unsigned GuardsOK = 1, ChainOK = 1, Consistent = 1, Iterations = 0;
  unsigned Round, I;
#if defined(_M_IX86)
  const unsigned Chain = __readfsdword(0);
#endif
  for (Round = 0; Round != 32; ++Round) {
    for (I = 0; I != 16; ++I)
      Guards[I] = 0xC0DE0000u ^ (Round * 16 + I);
    Values[0] = nonlocal_finally_goto(0, 0);
    Values[1] = nonlocal_finally_goto(1, 0);
    Values[2] = nonlocal_finally_goto(0, 1);
    Values[3] = nonlocal_finally_goto(1, 1);
    Values[4] = nonlocal_finally_return(0);
    Values[5] = nonlocal_finally_return(1);
    for (I = 0; I != 6; ++I)
      if (Round == 0)
        FirstValues[I] = Values[I];
      else
        Consistent &= FirstValues[I] == Values[I];
    Iterations += 6;
    for (I = 0; I != 16; ++I)
      GuardsOK &= Guards[I] == (0xC0DE0000u ^ (Round * 16 + I));
#if defined(_M_IX86)
    ChainOK &= Chain == __readfsdword(0);
#endif
  }
  printf("NONLOCAL_FINALLY %u %u %u %u %u %u guards=%u chain=%u "
         "consistent=%u iterations=%u\n",
         Values[0], Values[1], Values[2], Values[3], Values[4], Values[5],
         GuardsOK, ChainOK, Consistent, Iterations);
  return !(GuardsOK && ChainOK && Consistent);
}

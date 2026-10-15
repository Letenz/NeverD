//===- native_seh_bridge_runtime.c - Native x64 bridge observations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe a fixed native-frame experiment independently of NeverD.

#include <stdio.h>
#include <windows.h>

#if !defined(_M_X64)
#error This protocol experiment requires native Windows x64.
#endif

#ifndef BRIDGE_CORRUPTION_PROBE
#define BRIDGE_CORRUPTION_PROBE 0
#endif

extern unsigned neverd_native_resume_probe(unsigned Leave, unsigned Throwing,
                                           unsigned *Observation);

extern volatile unsigned neverd_native_bridge_observed[7];

static int provider_path(HMODULE Runtime, const char *Symbol, char *Path) {
  union {
    FARPROC Function;
    LPCSTR Address;
  } Entry;
  HMODULE Provider;
  DWORD Length;
  Entry.Function = GetProcAddress(Runtime, Symbol);
  if (!Entry.Function ||
      !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          Entry.Address, &Provider))
    return 0;
  Length = GetModuleFileNameA(Provider, Path, MAX_PATH);
  return Length && Length < MAX_PATH;
}

int main(void) {
  volatile unsigned Guards[3] = {0xC01D0001u, 0xC01D0002u, 0xC01D0003u};
  unsigned Observed[8] = {0}, First[8] = {0};
  unsigned RegistersOK = 1, StackOK = 1, GuardsOK = 1, Consistent = 1;
  unsigned CallbacksOK = 1, CodesOK = 1;
  unsigned Round, I;
  char RuntimePath[MAX_PATH], UnwindPath[MAX_PATH], HandlerPath[MAX_PATH];
  HMODULE Runtime;
  DWORD PathLength;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  if (BRIDGE_CORRUPTION_PROBE) {
    fputs("NATIVE_SEH_BRIDGE_STARTED\n", stderr);
    fflush(stderr);
  }
  for (Round = 0; Round != 32; ++Round) {
    for (I = 0; I != 8; ++I) {
      unsigned Observation[6] = {0xC01D0011u, 0, 0, 0, 0, 0xC01D0012u};
      Observed[I] = neverd_native_resume_probe((I & 1) | ((I >> 2) << 1),
                                               (I >> 1) & 1, &Observation[1]);
      CallbacksOK &= neverd_native_bridge_observed[0] == 2;
      CallbacksOK &= neverd_native_bridge_observed[1] == ((I >> 1) & 1);
      CallbacksOK &= neverd_native_bridge_observed[2] == 0;
      CodesOK &= neverd_native_bridge_observed[3] ==
                 (((I >> 1) & 1) ? 0xe0424242u : 0);
      CodesOK &= neverd_native_bridge_observed[4] == 0xe0424243u;
      CodesOK &= neverd_native_bridge_observed[5] == (((I >> 1) & 1) + 1);
      CodesOK &= neverd_native_bridge_observed[6] == 0;
      Consistent &= Observed[I] == Observation[1];
      RegistersOK &= Observation[2];
      StackOK &= Observation[3];
      GuardsOK &= Observation[4];
      GuardsOK &=
          Observation[0] == 0xC01D0011u && Observation[5] == 0xC01D0012u;
      GuardsOK &= Guards[0] == 0xC01D0001u && Guards[1] == 0xC01D0002u &&
                  Guards[2] == 0xC01D0003u;
      if (Round == 0)
        First[I] = Observed[I];
      else
        Consistent &= First[I] == Observed[I];
    }
  }
  Runtime = GetModuleHandleA("vcruntime140.dll");
  PathLength = GetModuleFileNameA(Runtime, RuntimePath, sizeof(RuntimePath));
  if (!PathLength || PathLength >= sizeof(RuntimePath) ||
      !provider_path(Runtime, "_local_unwind", UnwindPath) ||
      !provider_path(Runtime, "__C_specific_handler", HandlerPath))
    return 2;
  printf("NATIVE_SEH_BRIDGE %u %u %u %u %u %u %u %u registers=%u stack=%u "
         "guards=%u "
         "callbacks=%u codes=%u consistent=%u iterations=256 runtime=%s "
         "unwind=%s handler=%s\n",
         Observed[0], Observed[1], Observed[2], Observed[3], Observed[4],
         Observed[5], Observed[6], Observed[7], RegistersOK, StackOK, GuardsOK,
         CallbacksOK, CodesOK, Consistent, RuntimePath, UnwindPath,
         HandlerPath);
  return !(RegistersOK && StackOK && GuardsOK && CallbacksOK && CodesOK &&
           Consistent);
}

//===- native_seh_resume_runtime.c - Native x64 resume observations -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe an experimental LLVM/CRT resume protocol, independently of NeverD.

#include <stdio.h>
#include <windows.h>

#if !defined(_M_X64)
#error This protocol experiment requires native Windows x64.
#endif

extern unsigned neverd_native_resume_probe(unsigned Leave, unsigned Throwing,
                                           unsigned *Observation);

int main(void) {
  volatile unsigned Guards[3] = {0xC01D0001u, 0xC01D0002u, 0xC01D0003u};
  unsigned Observed[4] = {0}, First[4] = {0};
  unsigned RegistersOK = 1, StackOK = 1, GuardsOK = 1, Consistent = 1;
  unsigned Round, I;
  char RuntimePath[MAX_PATH];
  DWORD PathLength;
  for (Round = 0; Round != 32; ++Round) {
    for (I = 0; I != 4; ++I) {
      unsigned Observation[6] = {0xC01D0011u, 0, 0, 0, 0, 0xC01D0012u};
      Observed[I] = neverd_native_resume_probe(I & 1, I >> 1, &Observation[1]);
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
  PathLength = GetModuleFileNameA(GetModuleHandleA("vcruntime140.dll"),
                                  RuntimePath, sizeof(RuntimePath));
  if (!PathLength || PathLength >= sizeof(RuntimePath))
    return 2;
  printf("NATIVE_SEH_RESUME %u %u %u %u registers=%u stack=%u guards=%u "
         "consistent=%u iterations=128 runtime=%s\n",
         Observed[0], Observed[1], Observed[2], Observed[3], RegistersOK,
         StackOK, GuardsOK, Consistent, RuntimePath);
  return !(RegistersOK && StackOK && GuardsOK && Consistent);
}

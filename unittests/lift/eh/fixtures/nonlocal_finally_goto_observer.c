//===- nonlocal_finally_goto_observer.c - Observe MSVC parent jumps ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Supplies an outer search handler without changing the observed function.

typedef unsigned U32;
typedef void *Pointer;
__declspec(dllimport) Pointer __stdcall LoadLibraryA(const char *);
__declspec(dllimport) Pointer __stdcall GetProcAddress(Pointer, const char *);
__declspec(dllimport) Pointer __stdcall GetStdHandle(U32);
__declspec(dllimport) int __stdcall WriteFile(Pointer, const void *, U32, U32 *,
                                              Pointer);
__declspec(dllimport) void __stdcall ExitProcess(U32);
typedef U32(__cdecl *Probe)(U32, U32);

static U32 chain(void) {
  U32 Result;
  __asm__("movl %%fs:0,%0" : "=r"(Result));
  return Result;
}

static char *number(char *Out, U32 Value) {
  char Digits[16];
  U32 Count = 0;
  do {
    Digits[Count++] = (char)('0' + Value % 10);
    Value /= 10;
  } while (Value);
  while (Count)
    *Out++ = Digits[--Count];
  *Out++ = ' ';
  return Out;
}

void probe_entry(void) {
  Pointer Module = LoadLibraryA("target.dll");
  if (!Module)
    ExitProcess(100);
  Probe Function = (Probe)GetProcAddress(Module, "nonlocal_finally_goto");
  if (!Function)
    ExitProcess(101);
  volatile U32 Guards[2] = {0x51a7e001, 0x51a7e002};
  U32 Values[4] = {0}, Caught = 0;
  const U32 Expected[4] = {123457, 12357, 13567, 1357};
  U32 ChainOK = 1, ValuesOK = 1, Iterations = 0;
  for (U32 Index = 0; Index != 32; ++Index) {
    for (U32 Case = 0; Case != 4; ++Case) {
      __try {
        U32 Before = chain();
        Values[Case] = Function(Case & 1, Case >> 1);
        ChainOK &= Before == chain();
        ValuesOK &= Values[Case] == Expected[Case];
        ++Iterations;
      } __except (1) {
        ++Caught;
      }
    }
  }
  U32 GuardOK = Guards[0] == 0x51a7e001 && Guards[1] == 0x51a7e002;
  char Buffer[160];
  char *Out = Buffer;
  for (U32 Case = 0; Case != 4; ++Case)
    Out = number(Out, Values[Case]);
  Out = number(Out, Caught);
  Out = number(Out, ChainOK);
  Out = number(Out, GuardOK);
  Out = number(Out, ValuesOK);
  Out = number(Out, Iterations);
  Out = number(Out, (U32)Module);
  *Out++ = '\n';
  U32 Written = 0;
  WriteFile(GetStdHandle((U32)-11), Buffer, (U32)(Out - Buffer), &Written, 0);
  ExitProcess(Caught || !ChainOK || !GuardOK || !ValuesOK || Iterations != 128);
}

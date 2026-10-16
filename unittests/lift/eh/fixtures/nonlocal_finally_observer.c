//===- nonlocal_finally_observer.c - Observe native MSVC parent returns ---===//
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
typedef U32(__cdecl *Probe)(U32);

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
  Probe Function = (Probe)GetProcAddress(Module, "nonlocal_finally_return");
  if (!Function)
    ExitProcess(101);
  volatile U32 Guards[2] = {0x51a7e001, 0x51a7e002};
  U32 Normal = 0, Throwing = 0, Caught = 0;
  U32 ChainOK = 1, ValuesOK = 1, Iterations = 0;
  for (U32 Index = 0; Index != 32; ++Index) {
    __try {
      U32 Before = chain();
      Normal = Function(0);
      ChainOK &= Before == chain();
      Throwing = Function(1);
      ChainOK &= Before == chain();
      ValuesOK &= Normal == 123 && Throwing == 13;
      Iterations += 2;
    } __except (1) {
      ++Caught;
    }
  }
  U32 GuardOK = Guards[0] == 0x51a7e001 && Guards[1] == 0x51a7e002;
  char Buffer[160];
  char *Out = Buffer;
  Out = number(Out, Normal);
  Out = number(Out, Throwing);
  Out = number(Out, Caught);
  Out = number(Out, ChainOK);
  Out = number(Out, GuardOK);
  Out = number(Out, ValuesOK);
  Out = number(Out, Iterations);
  Out = number(Out, (U32)Module);
  *Out++ = '\n';
  U32 Written = 0;
  WriteFile(GetStdHandle((U32)-11), Buffer, (U32)(Out - Buffer), &Written, 0);
  ExitProcess(Caught || !ChainOK || !GuardOK || !ValuesOK || Iterations != 64);
}

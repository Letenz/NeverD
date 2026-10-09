//===- unpack_delay.c - An independently linked delay-load program --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;

#ifdef DELAY_PROVIDER
__declspec(dllexport) U32 first(void) { return 7; }
__declspec(dllexport) U32 second(void) { return 43; }
int dll_entry(void *Base, U32 Reason, void *Reserved) { return 1; }
#else
__declspec(dllimport) U32 first(void);
__declspec(dllimport) U32 second(void);
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
extern U8 __ImageBase;

// A small fixture helper implements the documented linker/helper ABI. Each
// unresolved slot consults the shared module handle before looking up its
// own symbol, so a retained guest handle cannot pass as fresh loader state.
struct DelayDescriptor {
  U32 Attributes, Name, Handle, IAT, INT, Bound, Unload, TimeStamp;
};
volatile U32 DelayCalls;
void *__delayLoadHelper2(const struct DelayDescriptor *D, void **Slot) {
  ++DelayCalls;
  U8 *Base = &__ImageBase;
  void **Handle = (void **)(Base + D->Handle);
  if (!*Handle)
    *Handle = LoadLibraryA((const char *)(Base + D->Name));
  U64 Index = Slot - (void **)(Base + D->IAT);
  U64 Symbol = ((U64 *)(Base + D->INT))[Index];
  const char *Name = Symbol >> 63 ? (const char *)(Symbol & 0xffff)
                                  : (const char *)(Base + Symbol + 2);
  void *Address = GetProcAddress(*Handle, Name);
  if (!Address)
    ExitProcess(91);
  *Slot = Address;
  return Address;
}

struct PackRecord {
  U32 Bytes, ResolveAll;
  U8 Code[4096];
};
#pragma section(".pay", read, write)
__declspec(allocate(".pay")) struct PackRecord Pack = {0};

__declspec(dllexport) __attribute__((section(".prog"), noinline)) void
program(void) {
  if (first() != 7 || DelayCalls != (Pack.ResolveAll ? 2 : 1))
    ExitProcess(91);
  U32 Result = second();
  ExitProcess(DelayCalls == 2 ? Result : 91);
}

__declspec(dllexport) __attribute__((noinline)) void loader(void) {
  if (first() != 7 || (Pack.ResolveAll && second() != 43))
    ExitProcess(91);
  volatile U8 *Code = (volatile U8 *)program;
  for (U32 I = 0; I < Pack.Bytes; ++I)
    Code[I] = Pack.Code[I] ^ 0xa5;
  program();
}
#endif

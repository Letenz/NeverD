//===- windows_generated_tls.c - TLS materializes a later callback -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned int U32;
typedef unsigned long long U64;
typedef void (*Callback)(void *, U32, void *);
__declspec(dllimport) void ExitProcess(U32);
extern unsigned char __ImageBase;
void entry(void);

static volatile U32 Result;
static volatile U32 StartupCount;
static volatile U32 Notifications;
static U32 Index;
static const U32 TLSRaw[] = {11, 22};
// The fixture linker gives this zero-filled section execute permission.
__declspec(align(8)) unsigned char Generated[32];
__declspec(align(8)) unsigned char GeneratedEntry[16];

static volatile U32 *threadLocalData(void) {
  void **Vector;
#ifdef _M_X64
  __asm__("movq %%gs:0x58, %0" : "=r"(Vector));
#else
  __asm__("ldr %0, [x18, #0x58]" : "=r"(Vector));
#endif
  return (volatile U32 *)Vector[Index];
}

static void store32(unsigned Offset, U32 Value) {
  for (unsigned I = 0; I < 4; ++I)
    Generated[Offset + I] = Value >> (I * 8);
}
static void entry32(unsigned Offset, U32 Value) {
  for (unsigned I = 0; I < 4; ++I)
    GeneratedEntry[Offset + I] = Value >> (I * 8);
}

static void initialize(void *Module, U32 Reason, void *Reserved) {
  if (Reason != 1) {
    ++Notifications;
    return;
  }
  ++StartupCount;
  threadLocalData()[0] = 0x13579bdf;
#ifdef _M_X64
  // mov dword ptr [rip + Result], 42; ret
  Generated[0] = 0xc7;
  Generated[1] = 0x05;
  store32(2, (U64)&Result - (U64)(Generated + 10));
  store32(6, 42);
  Generated[10] = 0xc3;
  // The generated entry tail-jumps to the independent linked test body.
  GeneratedEntry[0] = 0xe9;
  entry32(1, (U64)entry - (U64)(GeneratedEntry + 5));
#else
  // ldr x16, literal; mov w17, #42; str w17, [x16]; ret
  store32(0, 0x58000090);
  store32(4, 0x52800551);
  store32(8, 0xb9000211);
  store32(12, 0xd65f03c0);
  store32(16, (U64)&Result);
  store32(20, (U64)&Result >> 32);
  entry32(0, 0x14000000 | (((U64)entry - (U64)GeneratedEntry) / 4 & 0x3ffffff));
#endif
}
static const Callback Callbacks[] = {initialize, (Callback)Generated, 0};
#pragma section(".rdata$T", read)
__declspec(allocate(".rdata$T")) const struct {
  U64 Begin, End;
  U32 *Index;
  const Callback *Callbacks;
  U32 ZeroFill, Characteristics;
} _tls_used = {(U64)TLSRaw, (U64)(TLSRaw + 2), &Index, Callbacks, 0, 0};

void entry(void) {
  // Read the output's own TLS directory, so the test exercises any adapters
  // the rebuilder installed rather than calling the original function directly.
  unsigned char *Base = &__ImageBase;
  U32 PE = *(U32 *)(Base + 0x3c);
  U32 TLS = *(U32 *)(Base + PE + 24 + 112 + 9 * 8);
  volatile U32 *Template = *(volatile U32 **)(Base + TLS);
  if (threadLocalData()[0] != 0x13579bdf || Template[0] != 11 ||
      Template[1] != 22)
    ExitProcess(91);
  // Only process attach may restore the captured main-thread bytes. Other
  // notifications must preserve the caller's current TLS values.
  threadLocalData()[0] = 0x2468;
  const Callback *Current = *(const Callback **)(Base + TLS + 24);
  for (U32 Reason = 0; Reason <= 3; ++Reason) {
    if (Reason == 1)
      continue;
    for (unsigned I = 0; Current[I]; ++I)
      Current[I](Base, Reason, 0);
  }
  ExitProcess(Result == 42 && StartupCount == 1 && Notifications == 3 &&
                      threadLocalData()[0] == 0x2468
                  ? 37
                  : 91);
}

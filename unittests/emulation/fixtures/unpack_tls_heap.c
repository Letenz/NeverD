//===- unpack_tls_heap.c - Generated entry with a TLS-only heap root ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
typedef unsigned long long U64;
#define NEVERD_GENERATED_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_GENERATED_MODE(Name, Value) enum { Name##Mode = Value };
#define NEVERD_GENERATED_TEXT(Name, Text) static const char Name[] = Text;
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_TEXT
#undef NEVERD_GENERATED_MODE
#undef NEVERD_GENERATED_VALUE
#include "unpack_pointer_state.h"
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, U32, U64);
__declspec(dllimport) int HeapFree(void *, U32, void *);

#pragma section(".tls", read, write)
__declspec(allocate(".tls")) static U8 InitialTLS[16] = {[0] = 0x5a,
                                                         [15] = 0xa5};
static U32 TLSIndex;
#pragma section(".rdata$T", read)
__declspec(allocate(".rdata$T")) const struct {
  const void *Start, *End;
  U32 *Index;
  const void *Callbacks;
  U32 ZeroFill, Characteristics;
} _tls_used = {InitialTLS, InitialTLS + sizeof(InitialTLS), &TLSIndex, 0, 0, 0};

static volatile U8 *tlsBytes(void) {
  void **Vector;
#ifdef _M_X64
  __asm__("movq %%gs:0x58, %0" : "=r"(Vector));
#else
  __asm__("ldr %0, [x18, #0x58]" : "=r"(Vector));
#endif
  return (volatile U8 *)Vector[TLSIndex];
}
// Byte access deliberately leaves the sole persistent root unaligned.
static U64 heapRoot(void) {
  U64 Value = 0;
  for (unsigned I = 0; I != 8; ++I)
    Value |= (U64)tlsBytes()[I + 3] << (I * 8);
  return Value;
}
static void setHeapRoot(U64 Value) {
  for (unsigned I = 0; I != 8; ++I)
    tlsBytes()[I + 3] = (U8)(Value >> (I * 8));
}

struct PackRecord {
  U32 Mode, Key, ProgramBytes, RelayBytes;
  U8 Program[Capacity], Relay[Capacity];
};
#pragma section(".pay", read, write)
__declspec(allocate(".pay")) struct PackRecord Pack = {0};

__attribute__((section(".prog$a"), noinline)) U32 program(void) {
  U32 Status = ExitStatus, Written = 0;
  if (tlsBytes()[0] != 0x5a || tlsBytes()[15] != 0xa5)
    Status = FailureStatus;
  if (Pack.Mode == HeapStateMode &&
      *(volatile U32 *)heapRoot() != InitializeResult)
    Status = FailureStatus;
  if (Pack.Mode == LateEncodedPointerMode)
    setHeapRoot(encodeNull(Pack.Mode));
  if ((Pack.Mode == EncodedPointerMode ||
       Pack.Mode == NativeEncodedPointerMode ||
       Pack.Mode == LateEncodedPointerMode) &&
      !decodesToNull(Pack.Mode, heapRoot()))
    Status = FailureStatus;
  if (!WriteFile(GetStdHandle(StdoutSelector), Message, sizeof(Message) - 1,
                 &Written, 0) ||
      Written != sizeof(Message) - 1)
    Status = FailureStatus;
  ExitProcess(Status);
  return Status;
}

__declspec(dllexport) U32 loader(void) {
  if (Pack.Mode == DecodedPointerMode ||
      Pack.Mode == NativeDecodedPointerMode) {
    setHeapRoot(0x12345678);
    decodesToNull(Pack.Mode, heapRoot());
  }
  if (Pack.Mode == HeapStateMode || Pack.Mode == ReleasedHeapStateMode) {
    volatile U32 *State = (U32 *)HeapAlloc(GetProcessHeap(), 0, sizeof(*State));
    if (!State)
      ExitProcess(FailureStatus);
    *State = InitializeResult;
    setHeapRoot((U64)State);
    if (Pack.Mode == ReleasedHeapStateMode) {
      if (!HeapFree(GetProcessHeap(), 0, (void *)State))
        ExitProcess(FailureStatus);
      setHeapRoot(0);
    }
  }
  if (Pack.Mode == EncodedPointerMode ||
      Pack.Mode == NativeEncodedPointerMode ||
      Pack.Mode == ClearedEncodedPointerMode) {
    setHeapRoot(encodeNull(Pack.Mode));
    if (Pack.Mode == ClearedEncodedPointerMode)
      setHeapRoot(0);
  }
  U8 *Out = (U8 *)program;
  for (U32 I = 0; I < Pack.ProgramBytes; ++I)
    Out[I] = Pack.Program[I] ^ Pack.Key;
  __attribute__((musttail)) return program();
}

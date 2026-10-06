//===- unpack_generated.c - A program whose loader writes its code --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned char U8;
typedef unsigned int U32;
#define NEVERD_GENERATED_VALUE(Name, Value) enum { Name = Value };
#define NEVERD_GENERATED_MODE(Name, Value) enum { Name##Mode = Value };
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_MODE
#undef NEVERD_GENERATED_VALUE
#define NEVERD_GENERATED_TEXT(Name, Text) static const char Name[] = Text;
#include "UnpackGeneratedCases.def"
#undef NEVERD_GENERATED_TEXT
__declspec(dllimport) void ExitProcess(U32);
__declspec(dllimport) void *GetStdHandle(U32);
__declspec(dllimport) int WriteFile(void *, const void *, U32, U32 *, void *);

// The record a packer fills in; it is the only content of its section. As
// linked it is empty and the loader below is never reached.
struct PackRecord {
  U32 Mode, Key, ProgramBytes, RelayBytes;
  U8 Program[Capacity], Relay[Capacity];
};
#pragma section(".pay", read, write)
__declspec(allocate(".pay")) struct PackRecord Pack = {0};

// Each code section begins with the function a loader writes it for, so the
// address of that function is the address of the section.
#define PROGRAM_ENTRY __attribute__((section(".prog$a"), noinline))
#define PROGRAM_CODE __attribute__((section(".prog$m"), noinline))
#define RELAY_ENTRY __attribute__((section(".relay$a"), noinline))
typedef U32 (*Entry)(void);

// A call into the program that a loader makes before it leaves, as a system
// loader calls initializers. The volatile cell keeps the call observable to
// the compiler and holds the value it was linked with.
static volatile U32 Initialized = InitializeResult;
PROGRAM_CODE static U32 initialize(void) {
  Initialized = InitializeResult;
  return Initialized;
}

PROGRAM_CODE static U32 status(U32 Written) {
  U32 Sum = 0;
  for (U32 I = 0; I < Written; ++I)
    Sum += (U8)Message[I];
  return Sum ? ExitStatus : FailureStatus;
}

PROGRAM_ENTRY U32 program(void) {
  U32 Written = 0;
  WriteFile(GetStdHandle(StdoutSelector), Message, sizeof(Message) - 1,
            &Written, 0);
  ExitProcess(status(Written));
  return FailureStatus;
}

static void decode(Entry Target, const U8 *In, U32 Bytes) {
  U8 *Out = (U8 *)Target;
  for (U32 I = 0; I < Bytes; ++I)
    Out[I] = (U8)(In[I] ^ Pack.Key);
}

// The second loader is itself code the first loader wrote.
RELAY_ENTRY U32 relay(void) {
  decode(program, Pack.Program, Pack.ProgramBytes);
  __attribute__((musttail)) return program();
}

// Every path leaves by a jump on the stack the process started with, as a
// loader does when it hands control to the program it carried.
__declspec(dllexport) U32 loader(void) {
  if (Pack.Mode == StagedMode) {
    decode(relay, Pack.Relay, Pack.RelayBytes);
    __attribute__((musttail)) return relay();
  }
  decode(program, Pack.Program, Pack.ProgramBytes);
  if (Pack.Mode == CallMode && initialize() != InitializeResult)
    ExitProcess(FailureStatus);
  __attribute__((musttail)) return program();
}

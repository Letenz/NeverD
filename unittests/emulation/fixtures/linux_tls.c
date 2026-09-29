//===- linux_tls.c - Compiler-emitted local-exec TLS and real startup -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long U64;
#define NEVERD_TLS_VALUE(Name, Value) static const U64 Name = Value;
#define NEVERD_TLS_TEXT(Name, Text) static const char Name[] = Text;
#include "LinuxTLSCases.def"
#undef NEVERD_TLS_VALUE
#undef NEVERD_TLS_TEXT
#define NEVERD_LINUX_FIXTURE_SERVICE(Name, Number)                             \
  enum { Service##Name = Number };
#include "LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_SERVICE

extern U64 linux_service(U64 Number, U64 A0, U64 A1, U64 A2);
extern U64 tls_set_pointer(U64 Base);
static _Thread_local volatile U64 Initialized
    __attribute__((aligned(NEVERD_TLS_ALIGNMENT))) = NEVERD_TLS_INITIAL;
static _Thread_local volatile U64 Zero;
static unsigned char Blocks[2][NEVERD_TLS_BLOCK_SIZE]
    __attribute__((aligned(NEVERD_TLS_BLOCK_SIZE)));

struct ProgramHeader {
  unsigned Type, Flags;
  U64 Offset, VirtualAddress, PhysicalAddress, FileSize, MemorySize, Alignment;
};

static void finish(U64 Status) {
  linux_service(ServiceExit, Status, 0, 0);
  __builtin_trap();
}

static const struct ProgramHeader *tlsHeader(U64 *Stack) {
  U64 *Cursor = Stack + Stack[0] + 2;
  while (*Cursor)
    ++Cursor;
  ++Cursor;
  const struct ProgramHeader *Headers = 0;
  U64 Count = 0;
  for (U64 I = 0; I < MaxAuxEntries && Cursor[0]; ++I, Cursor += 2) {
    if (Cursor[0] == PhdrTag)
      Headers = (const struct ProgramHeader *)Cursor[1];
    if (Cursor[0] == PhnumTag)
      Count = Cursor[1];
  }
  if (Headers)
    for (U64 I = 0; I < Count; ++I)
      if (Headers[I].Type == TlsHeader)
        return &Headers[I];
  return 0;
}

static U64 prepare(const struct ProgramHeader *Header, unsigned Index) {
  U64 Align = Header->Alignment ? Header->Alignment : 1;
  if (Align > BlockAlignment || Header->MemorySize > BlockSize / 2 ||
      Header->FileSize > Header->MemorySize)
    finish(FailureStatus);
  unsigned char *Template;
  U64 Pointer;
#if defined(__aarch64__)
  // ELF variant I: an aligned thread pointer precedes the two-word TCB
  // and template. Preserve the template's alignment congruence.
  Pointer = (U64)Blocks[Index];
  Template = (unsigned char *)(Pointer + TcbWords * WordSize +
                               ((Header->VirtualAddress - TcbWords * WordSize) &
                                (Align - 1)));
#else
  // ELF variant II: the template and its alignment padding precede TP.
  Pointer = (U64)Blocks[Index] + BlockSize / 2;
  Template =
      (unsigned char *)(Pointer - Header->MemorySize -
                        ((0 - Header->VirtualAddress - Header->MemorySize) &
                         (Align - 1)));
  *(U64 *)Pointer = Pointer;
#endif
  const unsigned char *Source = (const unsigned char *)Header->VirtualAddress;
  for (U64 I = 0; I < Header->MemorySize; ++I)
    Template[I] = I < Header->FileSize ? Source[I] : 0;
  return Pointer;
}

// A compiler may retain TLS addresses across calls within one thread. Enter
// a separate body after switching TP so each logical thread obtains its own
// addresses; volatile alone does not invalidate a retained TLS address.
__attribute__((noinline)) static void
threadBody(U64 Pointer, U64 ExpectedInitial, U64 ExpectedZero, U64 NextInitial,
           U64 NextZero) {
  if ((U64)__builtin_thread_pointer() != Pointer ||
      Initialized != ExpectedInitial || Zero != ExpectedZero)
    finish(FailureStatus);
  Initialized = NextInitial;
  Zero = NextZero;
}

void process_main(U64 *Stack) {
  const struct ProgramHeader *Header = tlsHeader(Stack);
  if (!Header)
    finish(FailureStatus);
#if !defined(__aarch64__)
  U64 Seen = InvalidBase;
  if (linux_service(ArchPrctl, GetFS, (U64)&Seen, 0) || Seen)
    finish(FailureStatus);
  if (Stack[0] > 1 && ((const char *)Stack[2])[0] == Unknown[0]) {
    linux_service(ArchPrctl, UnsupportedOperation, 0, 0);
    finish(FailureStatus);
  }
#endif
  U64 Pointers[2] = {prepare(Header, 0), prepare(Header, 1)};
  if (tls_set_pointer(Pointers[0]))
    finish(FailureStatus);
  threadBody(Pointers[0], Initial, 0, First, Second);
  if (tls_set_pointer(Pointers[1]))
    finish(FailureStatus);
  threadBody(Pointers[1], Initial, 0, Second, First);
  if (tls_set_pointer(Pointers[0]))
    finish(FailureStatus);
  threadBody(Pointers[0], First, Second, First, Second);
#if !defined(__aarch64__)
  if (linux_service(ArchPrctl, GetFS, (U64)&Seen, 0) || Seen != Pointers[0] ||
      linux_service(ArchPrctl, NEVERD_TLS_SET_FS, InvalidBase, 0) !=
          (U64)0 - PermissionDenied ||
      linux_service(ArchPrctl, GetFS, BadPointer, 0) != (U64)0 - BadAddress ||
      linux_service(ArchPrctl, GetFS, (U64)Message, 0) != (U64)0 - BadAddress ||
      linux_service(ArchPrctl, GetFS, (U64)&Seen, 0) || Seen != Pointers[0] ||
      linux_service(ArchPrctl, SetGS, Pointers[1], 0) ||
      linux_service(ArchPrctl, GetGS, (U64)&Seen, 0) || Seen != Pointers[1] ||
      linux_service(ArchPrctl, SetGS, 0, 0))
    finish(FailureStatus);
#endif
  if (tls_set_pointer(Pointers[1]))
    finish(FailureStatus);
  threadBody(Pointers[1], Second, First, Second, First);
  if (linux_service(ServiceWrite, StandardOutput, (U64)Message,
                    sizeof(Message) - 1) != sizeof(Message) - 1 ||
      tls_set_pointer(0))
    finish(FailureStatus);
  finish(ExitStatus);
}

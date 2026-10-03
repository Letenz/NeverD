//===- linux_process.c - Compiler-built ELF process workload --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long U64;
#define NEVERD_LINUX_FIXTURE_VALUE(Name, Value) static const U64 Name = Value;
#define NEVERD_LINUX_FIXTURE_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_LINUX_FIXTURE_BYTES(Name, ...)                                  \
  static const unsigned char Name[] = {__VA_ARGS__};
#define NEVERD_LINUX_FIXTURE_MODE(Name, Character, Text)                       \
  enum { Name = Character };
#define NEVERD_LINUX_FIXTURE_SERVICE(Name, Value)                              \
  enum { Service##Name = Value };
#include "LinuxProcessCases.def"
#undef NEVERD_LINUX_FIXTURE_SERVICE
#undef NEVERD_LINUX_FIXTURE_MODE
#undef NEVERD_LINUX_FIXTURE_BYTES
#undef NEVERD_LINUX_FIXTURE_TEXT
#undef NEVERD_LINUX_FIXTURE_VALUE

extern U64 linux_service(U64 Number, U64 A0, U64 A1, U64 A2);
volatile U64 Initialized = NEVERD_LINUX_FIXTURE_INITIAL_DATA;
volatile U64 Zero;
unsigned char LastPage[NEVERD_LINUX_FIXTURE_PAGE_SIZE]
    __attribute__((aligned(NEVERD_LINUX_FIXTURE_PAGE_SIZE)));

static int equal(const char *A, const char *B) {
  while (*A && *A == *B) {
    ++A;
    ++B;
  }
  return *A == *B;
}
static U64 auxv(U64 *Values, U64 Key) {
  for (U64 I = 0; I < MaxAuxEntries && Values[0]; ++I, Values += 2)
    if (Values[0] == Key)
      return Values[1];
  return 0;
}
static void finish(U64 Status) {
  linux_service(ServiceExit, Status, 0, 0);
  __builtin_trap();
}
void process_main(U64 *Stack) {
  if (Stack[0] != ExpectedArgc || (U64)Stack % StackAlignment)
    finish(FailureStatus);
  const char **Args = (const char **)(Stack + 1);
  const char **Env = Args + Stack[0] + 1;
  if (!equal(Args[0], ExecutableName) || Args[Stack[0]] || !Env[0] ||
      !equal(Env[0], Environment) || Env[1])
    finish(FailureStatus);
  U64 *Aux = (U64 *)(Env + 2);
  if (auxv(Aux, PageszTag) != PageSize || !auxv(Aux, PhdrTag) ||
      auxv(Aux, PhentTag) != PhdrEntrySize || !auxv(Aux, RandomTag))
    finish(FailureStatus);
  const unsigned char *Random = (const unsigned char *)auxv(Aux, RandomTag);
  U64 Nonzero = 0;
  for (U64 I = 0; I < RandomSize; ++I)
    Nonzero |= Random[I];
  if (!Nonzero || Initialized != InitialData || Zero)
    finish(FailureStatus);
  Zero = 1;
  if (Args[1][0] == Loop)
    while (1)
      ++Zero;
  if (Args[1][0] == Fault)
    *(volatile U64 *)BadAddress = InitialData;
  if (Args[1][0] == ReadOnly)
    *(volatile char *)Message = 0;
  if (Args[1][0] == Unknown)
    linux_service(UnknownService, 0, 0, 0);
  if (Args[1][0] == SetIdentity)
    linux_service(ServiceSetUID, 0, 0, 0);
  if (Args[1][0] == IdentityQueries) {
    const U64 Services[] = {ServiceGetUID, ServiceGetEUID, ServiceGetGID,
                            ServiceGetEGID};
    const U64 Tags[] = {UIDTag, EUIDTag, GIDTag, EGIDTag};
    for (U64 I = 0; I < 4; ++I) {
      // Unused argument registers must not become the zero-argument result.
      U64 Value = linux_service(Services[I], ~(U64)0, BadAddress, InitialData);
      if (Value != Identity || Value != auxv(Aux, Tags[I]))
        finish(FailureStatus);
    }
    finish(ExitStatus);
  }
  if (Args[1][0] == Partial) {
    for (U64 I = 0; I < sizeof(Tail) - 1; ++I)
      LastPage[PageSize - (sizeof(Tail) - 1) + I] = Tail[I];
    U64 Written = linux_service(ServiceWrite, StandardOutput,
                                (U64)(LastPage + PageSize - (sizeof(Tail) - 1)),
                                2 * (sizeof(Tail) - 1));
    finish(Written == sizeof(Tail) - 1 ? ExitStatus : FailureStatus);
  }
  if (!linux_service(ServiceGetPID, 0, 0, 0) ||
      !linux_service(ServiceGetTID, 0, 0, 0) ||
      linux_service(ServiceWrite, BadFD, 0, 0) != (U64)0 - ErrorDescriptor ||
      linux_service(ServiceWrite, StandardOutput, BadAddress, 1) !=
          (U64)0 - ErrorFault ||
      linux_service(ServiceWrite, StandardOutput, BadAddress, 0) != 0)
    finish(FailureStatus);
  if (linux_service(ServiceWrite, StandardOutput, (U64)Message,
                    sizeof(Message) - 1) != sizeof(Message) - 1 ||
      linux_service(ServiceWrite, StandardError, (U64)BinaryOutput,
                    sizeof(BinaryOutput)) != sizeof(BinaryOutput))
    finish(FailureStatus);
  finish(ExitStatus);
}

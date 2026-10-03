//===- linux_output.c - Original Linux vectored output workload -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long long U64;
#define NEVERD_LINUX_OUTPUT_VALUE(Name, Value) static const U64 Name = Value;
#define NEVERD_LINUX_OUTPUT_TEXT(Name, Text) static const char Name[] = Text;
#define NEVERD_LINUX_OUTPUT_CASE(Name, Mode, Out, Err) enum { Name = Mode };
#define NEVERD_LINUX_OUTPUT_SERVICE(Name, Value) enum { Service##Name = Value };
#include "LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_SERVICE
#undef NEVERD_LINUX_OUTPUT_CASE
#undef NEVERD_LINUX_OUTPUT_TEXT
#undef NEVERD_LINUX_OUTPUT_VALUE

struct Vector {
  const void *Base;
  U64 Size;
};
extern U64 linux_service(U64 Number, U64 A0, U64 A1, U64 A2);
static unsigned char Pages[3 * NEVERD_LINUX_OUTPUT_PAGE_SIZE]
    __attribute__((aligned(NEVERD_LINUX_OUTPUT_PAGE_SIZE)));
static struct Vector Many[NEVERD_LINUX_OUTPUT_MAX_IOVECS];

static void finish(U64 Status) {
  linux_service(ServiceExit, Status, 0, 0);
  __builtin_trap();
}
static void check(U64 Actual, U64 Expected) {
  if (Actual != Expected)
    finish(FailureStatus);
}
static U64 output(U64 FD, const struct Vector *Vectors, U64 Count) {
  return linux_service(ServiceWriteV, FD, (U64)Vectors, Count);
}
void process_main(U64 *Stack) {
  if (Stack[0] != 2)
    finish(FailureStatus);
  const char **Args = (const char **)(Stack + 1);
  struct Vector Vectors[] = {
      {Message, sizeof(Message) - 1},
      {(const void *)BadAddress, 0},
      {Message + MessageSplit, sizeof(Message) - 1 - MessageSplit}};
  const U64 Bytes = sizeof(Message) - 1;
  switch (Args[1][0]) {
  case Gather:
    Vectors[0].Size = MessageSplit;
    check(output(StandardOutput, Vectors, 3), Bytes);
    Vectors[0].Base = ErrorBytes;
    Vectors[0].Size = sizeof(ErrorBytes) - 1;
    check(output(StandardError, Vectors, 1), sizeof(ErrorBytes) - 1);
    break;
  case Empty:
    check(output(BadDescriptor, (const struct Vector *)KernelAddress, 0),
          (U64)0 - ErrorDescriptor);
    check(output(StandardOutput, (const struct Vector *)KernelAddress, 0), 0);
    check(output(StandardOutput, Vectors + 1, 1), 0);
    Vectors[1].Base = (const void *)KernelAddress;
    check(output(StandardOutput, Vectors + 1, 1), (U64)0 - ErrorFault);
    break;
  case Metadata: {
    check(output(BadDescriptor, (const struct Vector *)BadAddress,
                 MaxVectors + 1),
          (U64)0 - ErrorDescriptor);
    check(output(StandardOutput, (const struct Vector *)BadAddress,
                 MaxVectors + 1),
          (U64)0 - ErrorInvalid);
    check(output(StandardOutput, (const struct Vector *)KernelAddress, 1),
          (U64)0 - ErrorFault);
    struct Vector *Table =
        (struct Vector *)(Pages + 2 * PageSize - sizeof(struct Vector));
    *Table = Vectors[0];
    check(linux_service(ServiceMprotect, (U64)(Pages + 2 * PageSize), PageSize,
                        0),
          0);
    check(output(StandardOutput, Table, 2), (U64)0 - ErrorFault);
    Table->Size = NegativeLength;
    check(output(StandardOutput, Table, 2), (U64)0 - ErrorInvalid);
    break;
  }
  case Lengths:
    Vectors[1].Size = NegativeLength;
    check(output(StandardOutput, Vectors, 2), (U64)0 - ErrorInvalid);
    Vectors[0].Base = (const void *)BadAddress;
    check(output(StandardOutput, Vectors, 2), (U64)0 - ErrorInvalid);
    check(output(StandardOutput, Vectors + 1, 1), (U64)0 - ErrorInvalid);
    break;
  case CrossTable: {
    struct Vector *Table = (struct Vector *)(Pages + PageSize - sizeof(void *));
    Table[0] = Vectors[0];
    Table[1] = Vectors[1];
    check(output(StandardOutput, Table, 2), Bytes);
    break;
  }
  case Partial: {
    const U64 TailBytes = sizeof(Tail) - 1;
    unsigned char *Start = Pages + PageSize - TailBytes;
    for (U64 I = 0; I < TailBytes; ++I)
      Start[I] = Tail[I];
    check(linux_service(ServiceMprotect, (U64)(Pages + PageSize), PageSize, 0),
          0);
    Vectors[1].Base = Start;
    Vectors[1].Size = 2 * TailBytes;
    check(output(StandardOutput, Vectors, 3), Bytes + TailBytes);
    Vectors[0].Base = Pages + PageSize;
    check(output(StandardOutput, Vectors, 1), (U64)0 - ErrorFault);
    break;
  }
  case Maximum:
    for (U64 I = 0; I < MaxVectors; ++I) {
      Many[I].Base = (const void *)BadAddress;
      Many[I].Size = 0;
    }
    Many[MaxVectors - 1] = Vectors[0];
    check(output(StandardOutput, Many, MaxVectors), Bytes);
    break;
  case Addresses:
    Vectors[1].Base = (const void *)KernelAddress;
    Vectors[1].Size = 1;
    check(output(StandardOutput, Vectors, 2), (U64)0 - ErrorFault);
    Vectors[1].Size = 0;
    check(output(StandardOutput, Vectors, 2), (U64)0 - ErrorFault);
    Vectors[0].Base = (const void *)BadAddress;
    check(output(StandardOutput, Vectors, 1), (U64)0 - ErrorFault);
    break;
  case ImportBeforeCap:
    Vectors[0].Size = MaxReadWriteSize;
    Vectors[1].Base = (const void *)KernelAddress;
    Vectors[1].Size = 1;
    check(output(StandardOutput, Vectors, 2), (U64)0 - ErrorFault);
    Vectors[1].Base = (const void *)BadAddress;
    Vectors[1].Size = NegativeLength;
    check(output(StandardOutput, Vectors, 2), (U64)0 - ErrorInvalid);
    break;
  case ArgumentWidths:
    check(output(HighArgument | StandardOutput,
                 (const struct Vector *)KernelAddress, HighArgument),
          0);
    check(output(HighArgument | StandardOutput, Vectors, HighArgument | 1),
          Bytes);
    check(linux_service(ServiceWrite, HighArgument | StandardOutput,
                        KernelAddress, 0),
          0);
    break;
  default:
    finish(FailureStatus);
  }
  finish(ExitStatus);
}

//===- android_output.c - Bionic vectored output and raw syscall errors ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
typedef unsigned long U64;
#define NEVERD_LINUX_OUTPUT_VALUE(Name, Value) static const U64 Name = Value;
#define NEVERD_LINUX_OUTPUT_TEXT(Name, Text) static const char Name[] = Text;
#include "LinuxOutputCases.def"
#undef NEVERD_LINUX_OUTPUT_TEXT
#undef NEVERD_LINUX_OUTPUT_VALUE

struct Vector {
  const void *Base;
  U64 Size;
};
extern long writev(int, const struct Vector *, int);
extern int *__errno(void);

U64 vectored_output(void) {
  struct Vector Vectors[] = {
      {Message, MessageSplit},
      {(const void *)BadAddress, 0},
      {Message + MessageSplit, sizeof(Message) - 1 - MessageSplit}};
  *__errno() = ErrnoSeed;
  if (writev(StandardOutput, Vectors, 3) != sizeof(Message) - 1 ||
      *__errno() != ErrnoSeed)
    return FailureStatus;
  if (writev(BadDescriptor, (const struct Vector *)BadAddress, 1) != -1 ||
      *__errno() != ErrorDescriptor)
    return FailureStatus;
  Vectors[0].Base = (const void *)BadAddress;
  if (writev(StandardOutput, Vectors, 1) != -1 || *__errno() != ErrorFault)
    return FailureStatus;
  *__errno() = ErrnoSeed;
  register U64 X0 __asm__("x0") = BadDescriptor;
  register U64 X1 __asm__("x1") = (U64)Vectors;
  register U64 X2 __asm__("x2") = 1;
  register U64 X8 __asm__("x8") = NativeARMWriteV;
  __asm__ volatile("svc #0"
                   : "+r"(X0)
                   : "r"(X1), "r"(X2), "r"(X8)
                   : "memory", "cc");
  if (X0 != (U64)0 - ErrorDescriptor || *__errno() != ErrnoSeed)
    return FailureStatus;
  return 0;
}

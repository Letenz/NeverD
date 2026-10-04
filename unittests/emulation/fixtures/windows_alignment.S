//===- windows_alignment.S - Original aligned SSE fault sites ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
.text
#define NEVERD_WINDOWS_ALIGNMENT_OPERATION(Name, ...) \
  .globl Name;                                       \
  .globl Name##Fault;                                \
  .globl Name##Resume;                               \
  Name:;                                             \
  movdqu AlignmentSeed(%rip), %xmm0;                 \
  Name##Fault:;                                     \
  __VA_ARGS__;                                       \
  Name##Resume:;                                    \
  movdqu %xmm0, (%rdx);                              \
  ret
#include "WindowsAlignmentCases.def"
#undef NEVERD_WINDOWS_ALIGNMENT_OPERATION

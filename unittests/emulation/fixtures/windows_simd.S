//===- windows_simd.S - Original faulting SSE with host state preserved ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#define NEVERD_WINDOWS_SIMD_VALUE(Name, Value) .equ Name, Value;
#include "WindowsSIMDCases.def"
#undef NEVERD_WINDOWS_SIMD_VALUE
.text
.globl SaveInitial
SaveInitial:
  fxsave64 (%rcx)
  retq

// Each original leaf saves the host FP state before loading the test image.
// The handler skips only the labelled faulting instruction. Both paths save
// the observed result and restore the host before returning to compiled C.
#define NEVERD_WINDOWS_SIMD_LEAF(Name, Prefix, Escape, Opcode, ModRM)           \
  .p2align WrapperAlignmentPower; .globl Run##Name, Fault##Name, Resume##Name;  \
  Run##Name:; fxsave64 (%r8); fxrstor64 (%rcx); movq %r9, %rcx;               \
  Fault##Name:; .byte Prefix, Escape, Opcode, ModRM;                          \
  Resume##Name:; fxsave64 (%rdx); fxrstor64 (%r8); retq;
#define NEVERD_SIMD_CASE(Name, Status, Left, Right, FaultStatus, MaskedStatus,  \
                         MaskedResult, RepairedResult, Prefix, Escape,        \
                         Opcode, ModRM)                                      \
  NEVERD_WINDOWS_SIMD_LEAF(Register##Name, Prefix, Escape, Opcode, ModRM)      \
  NEVERD_WINDOWS_SIMD_LEAF(Memory##Name, Prefix, Escape, Opcode, MemoryOperand)
#include "../X64SIMDExceptionCases.def"
#undef NEVERD_SIMD_CASE
#undef NEVERD_WINDOWS_SIMD_LEAF

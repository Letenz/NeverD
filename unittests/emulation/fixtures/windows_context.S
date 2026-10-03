//===- windows_context.S - Original caller register seeds ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#define NEVERD_CAPTURE_VALUE(Name, Value) .set Name, Value
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_VALUE
.text
.p2align 4
.globl CaptureFixture
CaptureFixture:
#if defined(__x86_64__)
  pushq %rbx
  pushq %rbp
  pushq %rsi
  pushq %rdi
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $X64SaveSize, %rsp
  movq %rcx, X64TargetSlot(%rsp)
  movq %rdx, X64BufferSlot(%rsp)
  movq %r8, X64OutputSlot(%rsp)
  movq %rsp, ObservationSP(%r8)
  leaq CaptureResume(%rip), %rax
  movq %rax, ObservationPC(%r8)
#ifdef NEVERD_CAPTURE_FP_PROBE
  fxsave64 X64FXSlot(%rsp)
#else
#define NEVERD_CAPTURE_VECTOR(Index, Low, High) \
  .if Index < X64VectorCount; movdqu %xmm##Index, X64FXSlot + Index * VectorBytes(%rsp); .endif
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_VECTOR
#endif
#ifdef NEVERD_CAPTURE_FP_PROBE
  fxrstor64 FPSeed(%rip)
#endif
#define NEVERD_CAPTURE_VECTOR(Index, Low, High) \
  .if Index < X64VectorCount; movdqu VectorSeeds + Index * VectorBytes(%rip), %xmm##Index; .endif
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_VECTOR
  movq %rdx, %rcx
  movq $X64FlagSeed, ObservationFlags(%r8)
  cmpq %rcx, %rcx
#define NEVERD_CAPTURE_X64_GPR(Name, Index, Value) movq $Value, %Name
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_X64_GPR
  callq *X64TargetSlot(%rsp)
.globl CaptureResume
CaptureResume:
#ifdef NEVERD_CAPTURE_FP_PROBE
  fxrstor64 X64FXSlot(%rsp)
#else
#define NEVERD_CAPTURE_VECTOR(Index, Low, High) \
  .if Index < X64VectorCount; movdqu X64FXSlot + Index * VectorBytes(%rsp), %xmm##Index; .endif
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_VECTOR
#endif
  addq $X64SaveSize, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rdi
  popq %rsi
  popq %rbp
  popq %rbx
  retq
#else
  sub sp, sp, #ARM64SaveSize
  str x0, [sp, #ARM64TargetSlot]
  str x1, [sp, #ARM64BufferSlot]
  str x2, [sp, #ARM64OutputSlot]
  stp x19, x20, [sp, #ARM64GPRSave]
  stp x21, x22, [sp, #ARM64GPRSave + VectorBytes]
  stp x23, x24, [sp, #ARM64GPRSave + 2 * VectorBytes]
  stp x25, x26, [sp, #ARM64GPRSave + 3 * VectorBytes]
  stp x27, x28, [sp, #ARM64GPRSave + 4 * VectorBytes]
  stp x29, x30, [sp, #ARM64GPRSave + 5 * VectorBytes]
  stp d8, d9, [sp, #ARM64VectorSave]
  stp d10, d11, [sp, #ARM64VectorSave + VectorBytes]
  stp d12, d13, [sp, #ARM64VectorSave + 2 * VectorBytes]
  stp d14, d15, [sp, #ARM64VectorSave + 3 * VectorBytes]
  mrs x16, fpcr
  str w16, [sp, #ARM64FPCRSaveSlot]
  mrs x16, fpsr
  str w16, [sp, #ARM64FPSRSaveSlot]
  mov x16, sp
  str x16, [x2, #ObservationSP]
  adr x16, CaptureResume
  str x16, [x2, #ObservationPC]
  str x18, [x2, #ObservationTEB]
  adrp x16, VectorSeeds
  add x16, x16, :lo12:VectorSeeds
#define NEVERD_CAPTURE_VECTOR(Index, Low, High) ldr q##Index, [x16, (Index * VectorBytes)]
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_VECTOR
  mov x16, #ARM64Control
  msr fpcr, x16
  mov w16, #(ARM64Status & 0xffff)
  movk w16, #(ARM64Status >> 16), lsl #16
  msr fpsr, x16
  mov x16, #ARM64FlagSeed
  cmp x16, x16
  str x16, [x2, #ObservationFlags]
#define NEVERD_CAPTURE_ARM64_GPR(Name, Index, Value) mov Name, (Value)
#include "WindowsContextCases.def"
#undef NEVERD_CAPTURE_ARM64_GPR
  ldr x0, [sp, #ARM64BufferSlot]
  ldr x16, [sp, #ARM64TargetSlot]
  blr x16
.globl CaptureResume
CaptureResume:
  ldr w16, [sp, #ARM64FPCRSaveSlot]
  msr fpcr, x16
  ldr w16, [sp, #ARM64FPSRSaveSlot]
  msr fpsr, x16
  ldp d8, d9, [sp, #ARM64VectorSave]
  ldp d10, d11, [sp, #ARM64VectorSave + VectorBytes]
  ldp d12, d13, [sp, #ARM64VectorSave + 2 * VectorBytes]
  ldp d14, d15, [sp, #ARM64VectorSave + 3 * VectorBytes]
  ldp x19, x20, [sp, #ARM64GPRSave]
  ldp x21, x22, [sp, #ARM64GPRSave + VectorBytes]
  ldp x23, x24, [sp, #ARM64GPRSave + 2 * VectorBytes]
  ldp x25, x26, [sp, #ARM64GPRSave + 3 * VectorBytes]
  ldp x27, x28, [sp, #ARM64GPRSave + 4 * VectorBytes]
  ldp x29, x30, [sp, #ARM64GPRSave + 5 * VectorBytes]
  add sp, sp, #ARM64SaveSize
  ret
#endif

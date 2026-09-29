//===- linux_memory.S - Full Linux mmap system-call argument transport ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#define NEVERD_LINUX_MEMORY_ASM(Name, Value) .equ Name, Value
#include "LinuxMemoryCases.def"
#undef NEVERD_LINUX_MEMORY_ASM
.text
.global linux_memory_service
.type linux_memory_service, %function
linux_memory_service:
#if defined(__aarch64__)
  mov x8, x0
  mov x0, x1
  mov x1, x2
  mov x2, x3
  mov x3, x4
  mov x4, x5
  mov x5, x6
  svc #0
  ret
#else
  mov %rdi, %rax
  mov %rsi, %rdi
  mov %rdx, %rsi
  mov %rcx, %rdx
  mov %r8, %r10
  mov %r9, %r8
  mov StackArgumentOffset(%rsp), %r9
  syscall
  ret
#endif
.size linux_memory_service, .-linux_memory_service
.section .note.GNU-stack,"",%progbits

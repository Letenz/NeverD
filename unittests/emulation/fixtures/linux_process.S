//===- linux_process.S - Real ELF entry and system-call wrappers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
.text
.global _start
.type _start, %function
_start:
#if defined(__aarch64__)
  mov x0, sp
  bl process_main
  brk #0
#else
  xor %ebp, %ebp
  mov %rsp, %rdi
  call process_main
  ud2
#endif
.size _start, .-_start

.global linux_service
.type linux_service, %function
linux_service:
#if defined(__aarch64__)
  mov x8, x0
  mov x0, x1
  mov x1, x2
  mov x2, x3
  svc #0
  ret
#else
  mov %rdi, %rax
  mov %rsi, %rdi
  mov %rdx, %rsi
  mov %rcx, %rdx
  syscall
  ret
#endif
.size linux_service, .-linux_service
.section .note.GNU-stack,"",%progbits

//===- linux_tls.S - Guest startup thread-pointer installation -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxTLSCases.def"
.text
.global tls_set_pointer
.type tls_set_pointer, %function
tls_set_pointer:
#if defined(__aarch64__)
  msr tpidr_el0, x0
  mov x0, #0
  ret
#else
  mov %rdi, %rsi
  mov $NEVERD_TLS_SET_FS, %edi
  mov $NEVERD_TLS_ARCH_PRCTL, %eax
  syscall
  ret
#endif
.size tls_set_pointer, .-tls_set_pointer
.section .note.GNU-stack,"",%progbits

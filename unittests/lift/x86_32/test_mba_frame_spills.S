// Carry-save identities whose operands are reloaded from private frame slots.
#if defined(__APPLE__) || defined(_WIN32)
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif
.text

.macro spilled_mba name, subtract=0
.globl \name
#if defined(__ELF__)
.type \name, @function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  subl $16, %esp
  movl 20(%esp), %eax
  movl 24(%esp), %edx
  movl %eax, 0(%esp)
  movl %edx, 4(%esp)
  movl 0(%esp), %eax
  movl 4(%esp), %edx
  xorl %edx, %eax
  movl 0(%esp), %ecx
  movl 4(%esp), %edx
  .if \subtract
    notl %ecx
  .endif
  andl %edx, %ecx
  shll $1, %ecx
  .if \subtract
    subl %ecx, %eax
  .else
    addl %ecx, %eax
  .endif
  addl $16, %esp
  retl
#if defined(__ELF__)
.size \name, .-\name
#endif
.endm

spilled_mba SYMBOL(mba_spilled_add)
spilled_mba SYMBOL(mba_spilled_sub), 1
#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

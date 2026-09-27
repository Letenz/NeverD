// Independent reloads must retain the identities of private frame values.
#if defined(__APPLE__)
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif
.text

.macro spilled_mba name, subtract=0
.globl \name
#if defined(__ELF__)
.type \name, %function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
.p2align 2
\name:
  sub sp, sp, #16
  str x0, [sp]
  str x1, [sp, #8]
  ldr x0, [sp]
  ldr x1, [sp, #8]
  ldr x2, [sp]
  ldr x3, [sp, #8]
  eor x0, x0, x1
  .if \subtract
    mvn x2, x2
  .endif
  and x2, x2, x3
  lsl x2, x2, #1
  .if \subtract
    sub x0, x0, x2
  .else
    add x0, x0, x2
  .endif
  add sp, sp, #16
  ret
#if defined(__ELF__)
.size \name, .-\name
#endif
.endm

spilled_mba SYMBOL(mba_spilled_add)
spilled_mba SYMBOL(mba_spilled_sub), 1
#if defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

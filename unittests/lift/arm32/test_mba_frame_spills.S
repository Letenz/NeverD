// Carry-save identities in both ARM and Thumb instruction modes.
.syntax unified
#if defined(__thumb__)
.thumb
#else
.arm
#endif
.text

.macro spilled_mba name, subtract=0
.globl \name
.type \name, %function
.p2align 2
#if defined(__thumb__)
.thumb_func
#endif
\name:
  sub sp, sp, #8
  str r0, [sp]
  str r1, [sp, #4]
  ldr r0, [sp]
  ldr r1, [sp, #4]
  ldr r2, [sp]
  ldr r3, [sp, #4]
  eor r0, r0, r1
  .if \subtract
    mvn r2, r2
  .endif
  and r2, r2, r3
  lsl r2, r2, #1
  .if \subtract
    sub r0, r0, r2
  .else
    add r0, r0, r2
  .endif
  add sp, sp, #8
  bx lr
.size \name, .-\name
.endm

spilled_mba mba_spilled_add
spilled_mba mba_spilled_sub, 1
.section .note.GNU-stack,"",%progbits

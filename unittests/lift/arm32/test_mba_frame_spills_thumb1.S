// Thumb-1 carry-save identities with explicit private-frame reloads.
.syntax unified
.thumb
.text

.macro spilled_mba name, subtract=0
.globl \name
.type \name, %function
.p2align 2
.thumb_func
\name:
  sub sp, sp, #8
  str r0, [sp]
  str r1, [sp, #4]
  ldr r0, [sp]
  ldr r1, [sp, #4]
  ldr r2, [sp]
  ldr r3, [sp, #4]
  eors r0, r1
  .if \subtract
    mvns r2, r2
  .endif
  ands r2, r3
  lsls r2, r2, #1
  .if \subtract
    subs r0, r2
  .else
    adds r0, r2
  .endif
  add sp, sp, #8
  bx lr
.size \name, .-\name
.endm

spilled_mba mba_spilled_add
spilled_mba mba_spilled_sub, 1
.section .note.GNU-stack,"",%progbits

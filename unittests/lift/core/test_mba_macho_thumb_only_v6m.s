.syntax unified
.section __TEXT,__text,regular,pure_instructions
.thumb
.p2align 1
.globl _unmarked_mba
_unmarked_mba:
  mov r2, r0
  eors r2, r1
  mov r3, r0
  ands r3, r1
  lsls r3, r3, #1
  adds r0, r2, r3
  bx lr

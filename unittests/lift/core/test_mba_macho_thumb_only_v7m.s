.syntax unified
.section __TEXT,__text,regular,pure_instructions
.thumb
.p2align 1
.globl _unmarked_mba
_unmarked_mba:
  eor.w r2, r0, r1
  and.w r0, r0, r1
  add.w r0, r2, r0, lsl #1
  bx lr

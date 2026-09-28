.syntax unified
.thumb
.section __TEXT,__text,regular,pure_instructions
.globl _unmarked_mba
_unmarked_mba:
  eor.w r2, r0, r1
  and.w r0, r0, r1
  add.w r0, r2, r0, lsl #1
  bx lr

.p2align 2
.arm
.globl _unmarked_arm_mba
_unmarked_arm_mba:
  eor r2, r0, r1
  and r0, r0, r1
  add r0, r2, r0, lsl #1
  bx lr

.syntax unified
.section __TEXT,__arm_mba,regular,pure_instructions
.arm
.p2align 2
.globl _arm_reloc_mba
_arm_reloc_mba:
  movw r3, :lower16:_marker
  eor r2, r0, r1
  and r0, r0, r1
  add r0, r2, r0, lsl #1
  bx lr

.section __TEXT,__thumb_reloc,regular,pure_instructions
.thumb
.p2align 1
.globl _thumb_reloc_mba
_thumb_reloc_mba:
  movw r3, :lower16:_marker
  eor.w r2, r0, r1
  and.w r0, r0, r1
  add.w r0, r2, r0, lsl #1
  bx lr

.section __DATA,__data
.p2align 2
.globl _marker
_marker:
  .long 0

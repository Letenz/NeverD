.syntax unified
.arm
.text
.p2align 2

// The assembler resolves a literal naming a function in its own section, so
// no relocation writes it: PC + literal is that function's address.
.globl arm_local_fp
.type arm_local_fp, %function
arm_local_fp:
  ldr r0, .Larm_local_fp_literal
.Larm_local_fp_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_local_fp_literal:
  .word arm_local_target - .Larm_local_fp_add - 8
.size arm_local_fp, .-arm_local_fp

// One path loads the literal; the other reaches the PC ADD with an opaque
// live-in R0, so the output is not an exact address.
.globl arm_local_fp_bypass
.type arm_local_fp_bypass, %function
arm_local_fp_bypass:
  cmp r1, #0
  bne .Larm_local_fp_bypass_add
  ldr r0, .Larm_local_fp_bypass_literal
.Larm_local_fp_bypass_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_local_fp_bypass_literal:
  .word arm_local_target - .Larm_local_fp_bypass_add - 8
.size arm_local_fp_bypass, .-arm_local_fp_bypass

// A literal that is only loaded stays a number.
.globl arm_local_plain_literal
.type arm_local_plain_literal, %function
arm_local_plain_literal:
  ldr r0, .Larm_local_plain_literal_word
  bx lr
.p2align 2
.Larm_local_plain_literal_word:
  .word arm_local_target - arm_local_plain_literal
.size arm_local_plain_literal, .-arm_local_plain_literal

// PC + literal past the end of the section names nothing.
.globl arm_local_outside
.type arm_local_outside, %function
arm_local_outside:
  ldr r0, .Larm_local_outside_literal
.Larm_local_outside_add:
  add r0, pc, r0
  bx lr
.p2align 2
.Larm_local_outside_literal:
  .word 0x100000
.size arm_local_outside, .-arm_local_outside

.type arm_local_target, %function
arm_local_target:
  add r0, r0, #1
  bx lr
.size arm_local_target, .-arm_local_target

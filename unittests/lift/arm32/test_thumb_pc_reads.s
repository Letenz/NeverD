.syntax unified
.thumb
.text

// Every instruction under test starts at the second halfword of a word,
// where T32's PC (address + 4) and the word-aligned literal base differ.

// `add r0, pc` at F+2 reads F+6.
.p2align 2
.globl thumb_add_pc
.type thumb_add_pc, %function
.thumb_func
thumb_add_pc:
  nop
  add r0, pc
  bx lr
.size thumb_add_pc, .-thumb_add_pc

// ADR adds its offset to the word-aligned PC: r0 and r1 name the word after
// the function (F+16), and r2 the word before it (F-4).
.p2align 2
thumb_adr_before:
  .word 0x11111111
.globl thumb_adr_forms
.type thumb_adr_forms, %function
.thumb_func
thumb_adr_forms:
  nop
  adr r0, thumb_adr_after
  nop
  adr.w r1, thumb_adr_after
  adr.w r2, thumb_adr_before
  bx lr
thumb_adr_after:
  .word 0x22222222
.size thumb_adr_forms, .-thumb_adr_forms

// A literal load at F+2 reads from the word-aligned base F+4.
.p2align 2
.globl thumb_literal_load
.type thumb_literal_load, %function
.thumb_func
thumb_literal_load:
  nop
  ldr r0, thumb_literal_word
  bx lr
  nop
thumb_literal_word:
  .word 0x33333333
.size thumb_literal_load, .-thumb_literal_load

// Thumb PIC data: an R_ARM_REL32 literal added to the PC at F+2.
.p2align 2
.globl thumb_pic_data
.type thumb_pic_data, %function
.thumb_func
thumb_pic_data:
  ldr r1, .Lthumb_pic_data_literal
.Lthumb_pic_data_add:
  add r1, pc
  ldr r0, [r1]
  bx lr
.p2align 2
.Lthumb_pic_data_literal:
  .word thumb_pic_target - (.Lthumb_pic_data_add + 4)
.size thumb_pic_data, .-thumb_pic_data

// A function in the same section: the assembler resolves the literal.
.p2align 2
.globl thumb_local_fp
.type thumb_local_fp, %function
.thumb_func
thumb_local_fp:
  ldr r0, .Lthumb_local_fp_literal
.Lthumb_local_fp_add:
  add r0, pc
  bx lr
.p2align 2
.Lthumb_local_fp_literal:
  .word thumb_fp_target - (.Lthumb_local_fp_add + 4)
.size thumb_local_fp, .-thumb_local_fp

.p2align 2
.type thumb_fp_target, %function
.thumb_func
thumb_fp_target:
  adds r0, #1
  bx lr
.size thumb_fp_target, .-thumb_fp_target

.data
.p2align 2
.globl thumb_pic_target
.type thumb_pic_target, %object
thumb_pic_target:
  .word 0x44444444
.size thumb_pic_target, 4

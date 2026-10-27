// Two GOTOFF dispatches behind one call/pop GOT model.  The second adds its
// entry to %ebx after one path replaced it, so a completed proposal stage
// rejects it as a table.  A stage that runs out of evidence after the first
// dispatch never reaches the second and cannot decide it.
.text
.p2align 2
.globl jt_i386_gotoff_stage_pair
.type jt_i386_gotoff_stage_pair, @function
jt_i386_gotoff_stage_pair:
  pushl %ebx
  call .Lstage_pair_pc
.Lstage_pair_pc:
  popl %ebx
  .byte 0x81, 0xc3                 // addl imm32, %ebx
.Lstage_pair_field:
  .long .Lstage_pair_field - .Lstage_pair_pc
  .reloc .Lstage_pair_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  movl 8(%esp), %ecx
  cmpl $3, %ecx
  ja .Lstage_pair_second
  movl .Lstage_pair_first_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_stage_pair_first
jt_i386_gotoff_stage_pair_first:
  jmp *%eax
.irp n,0,1,2,3
.Lstage_pair_first\n:
  movl $5300+\n, %eax
  popl %ebx
  ret
.endr
.Lstage_pair_second:
  testl $1, 12(%esp)
  je .Lstage_pair_keep
  movl 16(%esp), %ebx
.Lstage_pair_keep:
  subl $4, %ecx
  cmpl $3, %ecx
  ja .Lstage_pair_default
  movl .Lstage_pair_second_table@GOTOFF(%ebx,%ecx,4), %eax
  addl %ebx, %eax
.globl jt_i386_gotoff_stage_pair_second
jt_i386_gotoff_stage_pair_second:
  jmp *%eax
.irp n,0,1,2,3
.Lstage_pair_second\n:
  movl $5310+\n, %eax
  popl %ebx
  ret
.endr
.Lstage_pair_default:
  movl $-1, %eax
  popl %ebx
  ret
.size jt_i386_gotoff_stage_pair, .-jt_i386_gotoff_stage_pair

.section .rodata
.p2align 2
.Lstage_pair_first_table:
.irp n,0,1,2,3
  .long .Lstage_pair_first\n@GOTOFF
.endr
.Lstage_pair_second_table:
.irp n,0,1,2,3
  .long .Lstage_pair_second\n@GOTOFF
.endr

.section .note.GNU-stack,"",@progbits

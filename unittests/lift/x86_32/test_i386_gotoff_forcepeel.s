.text
.p2align 2
.globl jt_i386_gotoff_peeled_dec
.type jt_i386_gotoff_peeled_dec, @function
jt_i386_gotoff_peeled_dec:
  pushl %ebx
  movl 8(%esp), %eax
  call .Ldec_pc
.Ldec_pc:
  popl %ecx
  .byte 0x81, 0xc1
.Ldec_gotpc_field:
  .long .Ldec_gotpc_field - .Ldec_pc
  .reloc .Ldec_gotpc_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  orl $1, %eax
  movl %eax, %ebx
  andl $7, %ebx
  decl %ebx
  movl jt_i386_gotoff_peeled_dec_table@GOTOFF(%ecx,%ebx,4), %ebx
  addl %ecx, %ebx
.globl jt_i386_gotoff_peeled_dec_branch
jt_i386_gotoff_peeled_dec_branch:
  jmp *%ebx
.Ldec_case0:
  movl $10, %eax
  ret
.Ldec_case1:
  movl $11, %eax
  ret
.Ldec_case2:
  movl $12, %eax
  ret
.Ldec_case3:
  movl $13, %eax
  ret
.Ldec_case4:
  movl $14, %eax
  ret
.Ldec_case5:
  movl $15, %eax
  ret
.Ldec_case6:
  movl $16, %eax
  ret
.Ldec_case7:
  movl $17, %eax
  ret
.size jt_i386_gotoff_peeled_dec, .-jt_i386_gotoff_peeled_dec

.section .rodata
.p2align 2
.globl jt_i386_gotoff_peeled_dec_table
.type jt_i386_gotoff_peeled_dec_table, @object
jt_i386_gotoff_peeled_dec_table:
  .long .Ldec_case0@GOTOFF
  .long .Ldec_case1@GOTOFF
  .long .Ldec_case2@GOTOFF
  .long .Ldec_case3@GOTOFF
  .long .Ldec_case4@GOTOFF
  .long .Ldec_case5@GOTOFF
  .long .Ldec_case6@GOTOFF
  .long .Ldec_case7@GOTOFF
.size jt_i386_gotoff_peeled_dec_table, .-jt_i386_gotoff_peeled_dec_table

// Unique sized symbols for both tables, otherwise the same peeled-dec +
// adjacent loop table shape.  Isolates section-symbol ownership from `dec`.
.text
.p2align 2
.globl jt_i386_gotoff_peeled_dec_two
.type jt_i386_gotoff_peeled_dec_two, @function
jt_i386_gotoff_peeled_dec_two:
  pushl %ebx
  movl 8(%esp), %eax
  call .Ldec2_pc
.Ldec2_pc:
  popl %ecx
  .byte 0x81, 0xc1
.Ldec2_gotpc_field:
  .long .Ldec2_gotpc_field - .Ldec2_pc
  .reloc .Ldec2_gotpc_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  orl $1, %eax
  movl %eax, %ebx
  andl $7, %ebx
  decl %ebx
  movl jt_i386_gotoff_peeled_dec_two_first@GOTOFF(%ecx,%ebx,4), %ebx
  addl %ecx, %ebx
.globl jt_i386_gotoff_peeled_dec_two_first_branch
jt_i386_gotoff_peeled_dec_two_first_branch:
  jmp *%ebx
.Ldec2_loop:
  movl %eax, %ebx
  andl $7, %ebx
  movl jt_i386_gotoff_peeled_dec_two_second@GOTOFF(%ecx,%ebx,4), %ebx
  addl %ecx, %ebx
.globl jt_i386_gotoff_peeled_dec_two_loop_branch
jt_i386_gotoff_peeled_dec_two_loop_branch:
  jmp *%ebx
.Ldec2_case0:
  addl $11, %eax
  jmp .Ldec2_join
.Ldec2_case1:
  xorl $17, %eax
  jmp .Ldec2_join
.Ldec2_case2:
  addl $31, %eax
  jmp .Ldec2_join
.Ldec2_case3:
  xorl $33, %eax
  jmp .Ldec2_join
.Ldec2_case4:
  addl $59, %eax
  jmp .Ldec2_join
.Ldec2_case5:
  xorl $65, %eax
  jmp .Ldec2_join
.Ldec2_case6:
  addl $79, %eax
  jmp .Ldec2_join
.Ldec2_case7:
  xorl $81, %eax
.Ldec2_join:
  jmp .Ldec2_loop
.size jt_i386_gotoff_peeled_dec_two, .-jt_i386_gotoff_peeled_dec_two

.section .rodata
.p2align 2
.globl jt_i386_gotoff_peeled_dec_two_first
.type jt_i386_gotoff_peeled_dec_two_first, @object
jt_i386_gotoff_peeled_dec_two_first:
  .long .Ldec2_case0@GOTOFF
  .long .Ldec2_case1@GOTOFF
  .long .Ldec2_case2@GOTOFF
  .long .Ldec2_case3@GOTOFF
  .long .Ldec2_case4@GOTOFF
  .long .Ldec2_case5@GOTOFF
  .long .Ldec2_case6@GOTOFF
  .long .Ldec2_case7@GOTOFF
.size jt_i386_gotoff_peeled_dec_two_first, .-jt_i386_gotoff_peeled_dec_two_first
.globl jt_i386_gotoff_peeled_dec_two_second
.type jt_i386_gotoff_peeled_dec_two_second, @object
jt_i386_gotoff_peeled_dec_two_second:
  .long .Ldec2_case0@GOTOFF
  .long .Ldec2_case1@GOTOFF
  .long .Ldec2_case2@GOTOFF
  .long .Ldec2_case3@GOTOFF
  .long .Ldec2_case4@GOTOFF
  .long .Ldec2_case5@GOTOFF
  .long .Ldec2_case6@GOTOFF
  .long .Ldec2_case7@GOTOFF
.size jt_i386_gotoff_peeled_dec_two_second, .-jt_i386_gotoff_peeled_dec_two_second

// Clang i386 PIC -O2 forcepeel: two adjacent GOTOFF tables in one .rodata
// section, no sized table symbols, padding constants first, extra GOTOFF of
// the section start, and a peeled `and $7; dec` dispatch beside a loop
// `and $7` dispatch.  A unique-symbol sibling cannot stand in for this: the
// loader then owns an exact object boundary that clang never emits.
.text
.p2align 2
.globl jt_i386_gotoff_forcepeel_anon
.type jt_i386_gotoff_forcepeel_anon, @function
jt_i386_gotoff_forcepeel_anon:
  pushl %ebx
  pushl %esi
  movl 12(%esp), %eax
  call .Lfp_pc
.Lfp_pc:
  popl %ecx
  .byte 0x81, 0xc1
.Lfp_gotpc_field:
  .long .Lfp_gotpc_field - .Lfp_pc
  .reloc .Lfp_gotpc_field, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  orl $1, %eax
  movl %eax, %ebx
  andl $7, %ebx
  decl %ebx
  movl .Lfp_table1@GOTOFF(%ecx,%ebx,4), %ebx
  addl %ecx, %ebx
.globl jt_i386_gotoff_forcepeel_anon_first_branch
jt_i386_gotoff_forcepeel_anon_first_branch:
  jmp *%ebx
.Lfp_loop:
  movl %eax, %ebx
  andl $7, %ebx
  movl .Lfp_table2@GOTOFF(%ecx,%ebx,4), %ebx
  addl %ecx, %ebx
.globl jt_i386_gotoff_forcepeel_anon_loop_branch
jt_i386_gotoff_forcepeel_anon_loop_branch:
  jmp *%ebx
.Lfp_case0:
  addl $11, %eax
  jmp .Lfp_join
.Lfp_case1:
  xorl $17, %eax
  jmp .Lfp_join
.Lfp_case2:
  addl $31, %eax
  jmp .Lfp_join
.Lfp_case3:
  xorl $33, %eax
  jmp .Lfp_join
.Lfp_case4:
  addl $59, %eax
  jmp .Lfp_join
.Lfp_case5:
  xorl $65, %eax
  jmp .Lfp_join
.Lfp_case6:
  addl $79, %eax
  jmp .Lfp_join
.Lfp_case7:
  xorl $81, %eax
.Lfp_join:
  jmp .Lfp_loop
.size jt_i386_gotoff_forcepeel_anon, .-jt_i386_gotoff_forcepeel_anon

.section .rodata.fp_anon,"a",@progbits
.p2align 2
.Lfp_rodata:
  .long 0x3d11d946, 0x3d11d946, 0x3d11d946, 0x3d11d946
.Lfp_table1:
  .long .Lfp_case0@GOTOFF
  .long .Lfp_case1@GOTOFF
  .long .Lfp_case2@GOTOFF
  .long .Lfp_case3@GOTOFF
  .long .Lfp_case4@GOTOFF
  .long .Lfp_case5@GOTOFF
  .long .Lfp_case6@GOTOFF
.Lfp_table2:
  .long .Lfp_case0@GOTOFF
  .long .Lfp_case1@GOTOFF
  .long .Lfp_case2@GOTOFF
  .long .Lfp_case3@GOTOFF
  .long .Lfp_case4@GOTOFF
  .long .Lfp_case5@GOTOFF
  .long .Lfp_case6@GOTOFF
  .long .Lfp_case7@GOTOFF


.section .note.GNU-stack,"",@progbits

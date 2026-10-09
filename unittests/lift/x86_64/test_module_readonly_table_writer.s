        .text

// A store that may reach a read-only table faults before the switch could
// read a changed slot, so the table stays a static switch even though another
// function of the module writes through a pointer that walks it.  The same
// writer makes a writable table's switch unsafe
// (test_module_address_owner_induction.s).
        .globl  module_ro_owner_dispatch
        .type   module_ro_owner_dispatch,@function
module_ro_owner_dispatch:
        cmpl    $3, %edi
        ja      .Lmodule_ro_owner_default
        movl    %edi, %edi
        leaq    .Lmodule_ro_owner_table(%rip), %rax
        jmpq    *(%rax,%rdi,8)
.Lmodule_ro_owner_case0:
        movl    $7100, %eax
        retq
.Lmodule_ro_owner_case1:
        movl    $7101, %eax
        retq
.Lmodule_ro_owner_case2:
        movl    $7102, %eax
        retq
.Lmodule_ro_owner_case3:
        movl    $7103, %eax
        retq
.Lmodule_ro_owner_default:
        movl    $7199, %eax
        retq
        .size   module_ro_owner_dispatch, .-module_ro_owner_dispatch

        .globl  module_ro_owner_loop_writer
        .type   module_ro_owner_loop_writer,@function
module_ro_owner_loop_writer:
        leaq    .Lmodule_ro_owner_table(%rip), %rcx
        leaq    .Lmodule_ro_owner_table_end(%rip), %rdx
.Lmodule_ro_owner_loop:
        movq    %rax, (%rcx)
        addq    $8, %rcx
        cmpq    %rdx, %rcx
        jne     .Lmodule_ro_owner_loop
        retq
        .size   module_ro_owner_loop_writer, .-module_ro_owner_loop_writer

        .section .rodata.module_ro_owner_table,"a",@progbits
        .p2align 3
.Lmodule_ro_owner_table:
        .quad   .Lmodule_ro_owner_case0
        .quad   .Lmodule_ro_owner_case1
        .quad   .Lmodule_ro_owner_case2
        .quad   .Lmodule_ro_owner_case3
.Lmodule_ro_owner_table_end:

        .section .note.GNU-stack,"",@progbits

        .text

// GCC bounds a switch on a structure field with a compare against memory and
// loads the index again, as MinGW's _matherr does.  Nothing between the two
// loads writes memory, so they read one value and the guard bounds the index:
// slot 3 lies past the bound.
        .globl  jt_field_reload
        .type   jt_field_reload,@function
jt_field_reload:
        cmpl    $2, (%rdi)
        ja      .Lfield_default
        movl    (%rdi), %eax
        leaq    .Lfield_table(%rip), %rdx
        movslq  (%rdx,%rax,4), %rax
        addq    %rdx, %rax
        jmpq    *%rax
.Lfield_case0:
        movl    $600, %eax
        retq
.Lfield_case1:
        movl    $601, %eax
        retq
.Lfield_case2:
        movl    $602, %eax
        retq
.Lfield_poison:
        movl    $699, %eax
        retq
.Lfield_default:
        movl    $698, %eax
        retq
        .size   jt_field_reload, .-jt_field_reload

// A store between the compare and the reload may write the field: the reload
// is another value, which the guard does not bound.
        .globl  jt_field_reload_store
        .type   jt_field_reload_store,@function
jt_field_reload_store:
        cmpl    $2, (%rdi)
        ja      .Lstore_default
        movl    $3, (%rsi)
        movl    (%rdi), %eax
        leaq    .Lstore_table(%rip), %rdx
        movslq  (%rdx,%rax,4), %rax
        addq    %rdx, %rax
        jmpq    *%rax
.Lstore_case0:
        movl    $610, %eax
        retq
.Lstore_case1:
        movl    $611, %eax
        retq
.Lstore_case2:
        movl    $612, %eax
        retq
.Lstore_poison:
        movl    $619, %eax
        retq
.Lstore_default:
        movl    $618, %eax
        retq
        .size   jt_field_reload_store, .-jt_field_reload_store

// A load through another pointer is another value as well.
        .globl  jt_field_reload_other
        .type   jt_field_reload_other,@function
jt_field_reload_other:
        cmpl    $2, (%rdi)
        ja      .Lother_default
        movl    (%rsi), %eax
        leaq    .Lother_table(%rip), %rdx
        movslq  (%rdx,%rax,4), %rax
        addq    %rdx, %rax
        jmpq    *%rax
.Lother_case0:
        movl    $620, %eax
        retq
.Lother_case1:
        movl    $621, %eax
        retq
.Lother_case2:
        movl    $622, %eax
        retq
.Lother_poison:
        movl    $629, %eax
        retq
.Lother_default:
        movl    $628, %eax
        retq
        .size   jt_field_reload_other, .-jt_field_reload_other

        .section .rodata,"a",@progbits
        .p2align 2
.Lfield_table:
        .long   .Lfield_case0-.Lfield_table
        .long   .Lfield_case1-.Lfield_table
        .long   .Lfield_case2-.Lfield_table
        .long   .Lfield_poison-.Lfield_table
.Lstore_table:
        .long   .Lstore_case0-.Lstore_table
        .long   .Lstore_case1-.Lstore_table
        .long   .Lstore_case2-.Lstore_table
        .long   .Lstore_poison-.Lstore_table
.Lother_table:
        .long   .Lother_case0-.Lother_table
        .long   .Lother_case1-.Lother_table
        .long   .Lother_case2-.Lother_table
        .long   .Lother_poison-.Lother_table

// RBP is a borrowed frame/structure pointer, not this function's own frame.
// The compare and reload independently compute the same nonzero field address.
// Four physical table slots keep the unsigned guard essential to recovery.
        .text
        .macro OFFSET_RELOAD name, guard=36, reload=36, mode=0
        .globl \name
        .type \name,@function
\name:
        pushq   %rbp
        movq    %rdi, %rbp
        cmpl    $2, \guard(%rbp)
        ja      .Loffset_default\@
        .if \mode == 2
        addq    $4, %rbp
        .elseif \mode == 3
        movl    $3, (%rsi)
        .elseif \mode == 4
        callq   jt_field_reload_barrier
        .elseif \mode == 5
        mfence
        .elseif \mode == 6
        movq    %rsi, %rbp
        .endif
        .if \mode == 1
        movl    \reload(%rbp), %eax
        .elseif \mode == 7
        movzwl  \reload(%rbp), %eax
        .else
        movslq  \reload(%rbp), %rax
        .endif
        leaq    .Loffset_table\@(%rip), %rdx
        movslq  (%rdx,%rax,4), %rax
        addq    %rdx, %rax
        jmpq    *%rax
.Loffset_case0\@:
        movl    $630, %eax
        popq    %rbp
        retq
.Loffset_case1\@:
        movl    $631, %eax
        popq    %rbp
        retq
.Loffset_case2\@:
        movl    $632, %eax
        popq    %rbp
        retq
.Loffset_poison\@:
        movl    $639, %eax
        popq    %rbp
        retq
.Loffset_default\@:
        movl    $638, %eax
        popq    %rbp
        retq
        .size \name, .-\name
        .pushsection .rodata,"a",@progbits
        .p2align 2
.Loffset_table\@:
        .long .Loffset_case0\@-.Loffset_table\@
        .long .Loffset_case1\@-.Loffset_table\@
        .long .Loffset_case2\@-.Loffset_table\@
        .long .Loffset_poison\@-.Loffset_table\@
        .popsection
        .endm

        OFFSET_RELOAD jt_field_offset_sext
        OFFSET_RELOAD jt_field_offset_zext, 36, 36, 1
        OFFSET_RELOAD jt_field_offset_negative, -36, -36
        OFFSET_RELOAD jt_field_offset_different, 36, 40
        OFFSET_RELOAD jt_field_offset_changed_base, 36, 36, 2
        OFFSET_RELOAD jt_field_offset_store, 36, 36, 3
        OFFSET_RELOAD jt_field_offset_call, 36, 36, 4
        OFFSET_RELOAD jt_field_offset_fence, 36, 36, 5
        OFFSET_RELOAD jt_field_offset_other_base, 36, 36, 6
        OFFSET_RELOAD jt_field_offset_narrow, 36, 36, 7

        .type jt_field_reload_barrier,@function
jt_field_reload_barrier:
        movl    $3, 36(%rdi)
        retq
        .size jt_field_reload_barrier, .-jt_field_reload_barrier

        .section .note.GNU-stack,"",@progbits

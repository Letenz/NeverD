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

        .section .note.GNU-stack,"",@progbits

// Same finite domain as the AArch64 fixture, using CMOVcc and OR.
.macro begin name
.text
.p2align 4
.global \name
.type \name, @function
\name:
    subq $16, %rsp
    leaq \name\()_table(%rip), %rcx
.endm

.macro choose
    movl $0, %eax
    movl $1, %edx
    testl %edi, %edi
    cmovel %edx, %eax
    orl $2, %eax
.endm

.macro finish name, writable=0
    jmp *(%rcx,%rax,8)
\name\()_case0:
    movl $7, %eax
    addq $16, %rsp
    ret
\name\()_case1:
    movl $13, %eax
    addq $16, %rsp
    ret
\name\()_case2:
    movl $41, %eax
    addq $16, %rsp
    ret
\name\()_case3:
    movl $99, %eax
    addq $16, %rsp
    ret
.size \name, .-\name
.if \writable
.section .data,"aw",@progbits
.else
.section .rodata,"a",@progbits
.endif
.p2align 3
.type \name\()_table, @object
\name\()_table:
    .quad \name\()_case0, \name\()_case1
    .quad \name\()_case2, \name\()_case3
.size \name\()_table, .-\name\()_table
.endm

#if TEST_CASE == 0
begin finite_ro
choose
finish finite_ro
#elif TEST_CASE == 1
begin finite_rw
choose
finish finite_rw, 1
#elif TEST_CASE == 2
begin finite_unknown_arm
    xorl %eax, %eax
    testl %edi, %edi
    cmovel %esi, %eax
    orl $2, %eax
finish finite_unknown_arm
#elif TEST_CASE == 3
begin finite_bypassed
    testl %esi, %esi
    jnz finite_bypassed_dispatch
choose
finite_bypassed_dispatch:
finish finite_bypassed
#elif TEST_CASE == 4
begin finite_clobbered
choose
    movl %esi, %eax
finish finite_clobbered
#elif TEST_CASE == 5
begin finite_mutated
choose
    movq %rsi, 16(%rcx)
finish finite_mutated, 1
#endif

.section .note.GNU-stack,"",@progbits

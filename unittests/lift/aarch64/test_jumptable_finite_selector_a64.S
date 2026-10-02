// Independent finite selectors. Physical slots 0 and 1 are unreachable.
.macro begin name
.text
.p2align 2
.global \name
.type \name, %function
\name:
    sub sp, sp, #16
    adrp x8, \name\()_table
    add x8, x8, :lo12:\name\()_table
.endm

.macro choose
    cmp w0, #0
    cset w9, eq
    orr w9, w9, #2
.endm

.macro finish name, writable=0, repeat=0
    ldr x8, [x8, w9, uxtw #3]
    br x8
\name\()_case0:
    mov w0, #7
    add sp, sp, #16
    ret
\name\()_case1:
    mov w0, #13
    add sp, sp, #16
    ret
\name\()_case2:
.if \repeat
    // A reached destination changes the next selector.
.if \repeat == 2
    mov w9, #1
.else
    mov w9, w1
.endif
    adrp x8, \name\()_table
    add x8, x8, :lo12:\name\()_table
    b \name\()_dispatch
.else
    mov w0, #41
    add sp, sp, #16
    ret
.endif
\name\()_case3:
    mov w0, #99
    add sp, sp, #16
    ret
.size \name, .-\name
.if \writable
.section .data,"aw",%progbits
.else
.section .rodata,"a",%progbits
.endif
.p2align 3
.type \name\()_table, %object
\name\()_table:
#if TEST_CASE == 11 || TEST_CASE == 13
    .quad \name\()_case0, finite_callback
#else
    .quad \name\()_case0, \name\()_case1
#endif
#if TEST_CASE == 12
    .quad finite_callback, \name\()_case3
#else
    .quad \name\()_case2, \name\()_case3
#endif
#if TEST_CASE >= 7
    // A long physical run must neither grant nor prevent a small exact domain.
    .rept 92
    .quad \name\()_case0
    .endr
#endif
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
    cmp w0, #0
    csel w9, w1, wzr, eq
    orr w9, w9, #2
finish finite_unknown_arm
#elif TEST_CASE == 3
begin finite_bypassed
    cbnz w1, finite_bypassed_dispatch
choose
finite_bypassed_dispatch:
finish finite_bypassed
#elif TEST_CASE == 4
begin finite_clobbered
choose
    mov w9, w1
finish finite_clobbered
#elif TEST_CASE == 5
begin finite_mutated
choose
    str x1, [x8, #16]
finish finite_mutated, 1
#elif TEST_CASE == 6
begin finite_recurrent
choose
finite_recurrent_dispatch:
finish finite_recurrent, 0, 1
#elif TEST_CASE == 7
begin finite_large
choose
finish finite_large
#elif TEST_CASE == 8
begin finite_large_unknown
    mov w9, w1
    orr w9, w9, #2
finish finite_large_unknown
#elif TEST_CASE == 9
begin finite_above_ceiling
    cmp w0, #0
    cset w9, eq
    lsl w9, w9, #6
    orr w9, w9, #2
finish finite_above_ceiling
#elif TEST_CASE == 10
begin finite_large_rw
choose
finish finite_large_rw, 1
#elif TEST_CASE == 11
begin finite_foreign_prefix
choose
finish finite_foreign_prefix
#elif TEST_CASE == 12
begin finite_foreign_selected
choose
finish finite_foreign_selected
#elif TEST_CASE == 13
begin finite_foreign_recurrent
choose
finite_foreign_recurrent_dispatch:
finish finite_foreign_recurrent, 0, 2
#endif

#if TEST_CASE >= 11
.text
.p2align 2
.global finite_callback
.type finite_callback, %function
finite_callback:
    mov w0, #123
    ret
.size finite_callback, .-finite_callback
#endif

.section .note.GNU-stack,"",%progbits

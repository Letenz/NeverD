// A 64-bit result in edx:eax that a caller uses only in part, or returns
// itself after its epilogue.

        .text

// (uint64_t)a << 33: the low half is zero.
        .globl pair_wide
        .type pair_wide, @function
pair_wide:
        movl 4(%esp), %edx
        addl %edx, %edx
        xorl %eax, %eax
        ret
        .size pair_wide, .-pair_wide

// The high half of pair_wide's result alone.
        .globl pair_high
        .type pair_high, @function
pair_high:
        pushl 4(%esp)
        call pair_wide
        addl $4, %esp
        movl %edx, %eax
        ret
        .size pair_high, .-pair_high

// a + b.
        .globl pair_add
        .type pair_add, @function
pair_add:
        movl 4(%esp), %eax
        movl 8(%esp), %edx
        addl 12(%esp), %eax
        adcl 16(%esp), %edx
        ret
        .size pair_add, .-pair_add

// pair_add(pair_add(a, b), c): the second sum is the result, after the
// call's stack adjustment.
        .globl pair_sum3
        .type pair_sum3, @function
pair_sum3:
        pushl 16(%esp)
        pushl 16(%esp)
        pushl 16(%esp)
        pushl 16(%esp)
        call pair_add
        addl $16, %esp
        pushl 24(%esp)
        pushl 24(%esp)
        pushl %edx
        pushl %eax
        call pair_add
        addl $16, %esp
        ret
        .size pair_sum3, .-pair_sum3

// A helper that leaves edx alone, as one a register allocator sees may.
        .globl pair_keeps_edx_callee
        .type pair_keeps_edx_callee, @function
pair_keeps_edx_callee:
        movl $5, %eax
        ret
        .size pair_keeps_edx_callee, .-pair_keeps_edx_callee

// A caller that relies on that: the edx it reads is its own.
        .globl pair_keeps_edx
        .type pair_keeps_edx, @function
pair_keeps_edx:
        movl $7, %edx
        call pair_keeps_edx_callee
        addl %edx, %eax
        ret
        .size pair_keeps_edx, .-pair_keeps_edx

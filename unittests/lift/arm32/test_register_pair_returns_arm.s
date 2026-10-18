// A 64-bit result in r0:r1 that a caller uses only in part, passes on to the
// next call, or returns itself.

        .syntax unified
        .arch armv7-a
        .text
        .arm

// (uint64_t)a << 33: the low half is zero.
        .globl pair_wide
        .type pair_wide, %function
pair_wide:
        lsl r1, r0, #1
        mov r0, #0
        bx lr
        .size pair_wide, .-pair_wide

// The high half of pair_wide's result alone.
        .globl pair_high
        .type pair_high, %function
pair_high:
        push {r11, lr}
        bl pair_wide
        mov r0, r1
        pop {r11, pc}
        .size pair_high, .-pair_high

// a + b, in r0:r1 and r2:r3.
        .globl pair_add
        .type pair_add, %function
pair_add:
        adds r0, r0, r2
        adc r1, r1, r3
        bx lr
        .size pair_add, .-pair_add

// pair_add(pair_add(a, b), c): the first sum reaches the second call in
// r0:r1 untouched, and the second is a tail call.
        .globl pair_sum3
        .type pair_sum3, %function
pair_sum3:
        push {r11, lr}
        bl pair_add
        ldr r2, [sp, #8]
        ldr r3, [sp, #12]
        pop {r11, lr}
        b pair_add
        .size pair_sum3, .-pair_sum3

// The thread pointer, leaving r1 alone as __aeabi_read_tp does.
        .globl pair_read_tp
        .type pair_read_tp, %function
pair_read_tp:
        mrc p15, #0, r0, c13, c0, #3
        bx lr
        .size pair_read_tp, .-pair_read_tp

// A caller that relies on that: the r1 it reads is its own.
        .globl pair_keeps_r1
        .type pair_keeps_r1, %function
pair_keeps_r1:
        push {r11, lr}
        mov r1, #7
        bl pair_read_tp
        add r0, r0, r1
        pop {r11, pc}
        .size pair_keeps_r1, .-pair_keeps_r1

// A static image defines __aeabi_read_tp itself; a call to it keeps r1-r3
// as the import's does.

        .syntax unified
        .arch armv7-a
        .text
        .arm

        .globl __aeabi_read_tp
        .type __aeabi_read_tp, %function
__aeabi_read_tp:
        mrc p15, #0, r0, c13, c0, #3
        bx lr
        .size __aeabi_read_tp, .-__aeabi_read_tp

        .globl tls_bump_static
        .type tls_bump_static, %function
tls_bump_static:
        push {r11, lr}
        mov r1, r0
        ldr r2, .Ltls_bump_static_offset
        bl __aeabi_read_tp
        ldr r3, [r0, r2]
        add r1, r3, r1
        str r1, [r0, r2]
        mov r0, r1
        pop {r11, pc}
        .p2align 2
.Ltls_bump_static_offset:
        .word 0x14
        .size tls_bump_static, .-tls_bump_static

// clang's -mtp=soft local-exec TLS access: n stays in r1 and the variable's
// offset in r2 across __aeabi_read_tp, which corrupts only r0, ip, lr and the
// flags.  The routine is an import here.

        .syntax unified
        .arch armv7-a
        .text
        .arm

        .globl tls_bump
        .type tls_bump, %function
tls_bump:
        push {r11, lr}
        mov r1, r0
        ldr r2, .Ltls_bump_offset
        bl __aeabi_read_tp
        ldr r3, [r0, r2]
        add r1, r3, r1
        str r1, [r0, r2]
        mov r0, r1
        pop {r11, pc}
        .p2align 2
.Ltls_bump_offset:
        .word 0x14
        .size tls_bump, .-tls_bump

// The thread pointer itself.
        .globl get_tp
        .type get_tp, %function
get_tp:
        push {r11, lr}
        bl __aeabi_read_tp
        pop {r11, pc}
        .size get_tp, .-get_tp

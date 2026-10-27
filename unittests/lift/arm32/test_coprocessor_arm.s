// Coprocessor register transfers: MRC and MRRC read a system register into
// their registers, MCR and MCRR write one.

        .syntax unified
        .arch armv7-a
        .arch_extension virt
        .text

// TPIDRURO, the user read-only thread ID register, as Android's __get_tls
// reads it.
        .arm
        .globl read_tls
        .type read_tls, %function
read_tls:
        mrc p15, #0, r0, c13, c0, #3
        bx lr
        .size read_tls, .-read_tls

// The same read in Thumb, whose value is the base of a load.
        .thumb
        .thumb_func
        .globl tls_slot_thumb
        .type tls_slot_thumb, %function
tls_slot_thumb:
        mrc p15, #0, r1, c13, c0, #3
        ldr r0, [r1, #8]
        bx lr
        .size tls_slot_thumb, .-tls_slot_thumb

// TPIDRURW, the user read/write thread ID register, from the argument.
        .arm
        .globl write_tpidrurw
        .type write_tpidrurw, %function
write_tpidrurw:
        mcr p15, #0, r0, c13, c0, #2
        bx lr
        .size write_tpidrurw, .-write_tpidrurw

// The 64-bit virtual count, CNTVCT, which r0:r1 returns.
        .globl read_cntvct
        .type read_cntvct, %function
read_cntvct:
        mrrc p15, #1, r0, r1, c14
        bx lr
        .size read_cntvct, .-read_cntvct

// CNTV_CVAL from the 64-bit argument in r0:r1.
        .globl write_cntv_cval
        .type write_cntv_cval, %function
write_cntv_cval:
        mcrr p15, #3, r0, r1, c14
        bx lr
        .size write_cntv_cval, .-write_cntv_cval

// A read whose value goes unused still runs: reading DBGDTRRXint takes the
// word the debugger sent.
        .globl drain_dcc
        .type drain_dcc, %function
drain_dcc:
        mrc p14, #0, r1, c0, c5, #0
        mov r0, #0
        bx lr
        .size drain_dcc, .-drain_dcc

// The read of DBGDSCRint into the condition flags has no typed contract:
// the function is refused.
        .globl dscr_flags
        .type dscr_flags, %function
dscr_flags:
        mrc p14, #0, apsr_nzcv, c0, c1, #0
        movmi r0, #1
        movpl r0, #0
        bx lr
        .size dscr_flags, .-dscr_flags

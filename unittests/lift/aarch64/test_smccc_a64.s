// SMC and HVC #0 under the SMC Calling Convention: the function identifier
// and arguments in x0-x7, the results in x0-x3.

        .text
        .globl  smc_wrap
        .type   smc_wrap,%function
smc_wrap:
        smc     #0
        ret
        .size smc_wrap, .-smc_wrap

// Reads what smc_wrap returns: the call's x0.
        .globl  psci_version
        .type   psci_version,%function
psci_version:
        stp     x29, x30, [sp, #-16]!
        mov     x0, #0x84000000
        bl      smc_wrap
        add     x0, x0, #1
        ldp     x29, x30, [sp], #16
        ret
        .size psci_version, .-psci_version

// Reads the results in x1-x3, as TRNG_RND does.
        .globl  trng
        .type   trng,%function
trng:
        mov     x0, #0x53
        movk    x0, #0x8400, lsl #16
        smc     #0
        add     x0, x1, x2
        add     x0, x0, x3
        ret
        .size trng, .-trng

        .globl  hvc_call
        .type   hvc_call,%function
hvc_call:
        mov     x0, #0x86000000
        mov     x1, #7
        hvc     #0
        ret
        .size hvc_call, .-hvc_call

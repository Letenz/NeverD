// SMC #0 under the SMC Calling Convention: the function identifier and
// arguments in r0-r3, the results in r0-r3.

        .syntax unified
        .arch armv7-a
        .arch_extension sec
        .text
        .arm

        .globl smc_arm
        .type smc_arm, %function
smc_arm:
        movw r0, #0
        movt r0, #0x8400
        smc #0
        add r0, r0, r1
        bx lr
        .size smc_arm, .-smc_arm

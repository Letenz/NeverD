// A whole unit names a function the lift refuses, with why, where it would
// otherwise leave it out with no word: an exception return restores state
// no lift models.

    .syntax unified
    .text
    .arm

    .globl decompiled_arm
    .type decompiled_arm, %function
decompiled_arm:
    add r0, r0, #1
    bx lr
    .size decompiled_arm, .-decompiled_arm

    .globl refused_arm
    .type refused_arm, %function
refused_arm:
    rfeia sp
    .size refused_arm, .-refused_arm

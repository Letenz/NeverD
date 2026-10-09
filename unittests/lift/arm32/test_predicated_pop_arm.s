// Predicated push and pop.  Capstone spells them `popne`/`pushne` (and
// `popne.w` inside a Thumb-2 IT block), whose registers are all transferred
// through sp; none of them is a base register.

    .syntax unified
    .text

// int pred_return_arm(int x) { return x != 0 ? 7 : x + 1; }
    .arm
    .globl pred_return_arm
    .type pred_return_arm, %function
pred_return_arm:
    push {r4, lr}
    mov r4, r0
    cmp r4, #0
    movne r0, #7
    popne {r4, pc}
    add r0, r4, #1
    pop {r4, pc}
    .size pred_return_arm, .-pred_return_arm

// int pred_single_arm(int x) { return x != 0 ? 5 : 9; }, through a
// single-register predicated pop.
    .globl pred_single_arm
    .type pred_single_arm, %function
pred_single_arm:
    push {r4}
    mov r4, #9
    cmp r0, #0
    movne r4, #5
    mov r0, r4
    popne {r4}
    addeq sp, sp, #4
    bx lr
    .size pred_single_arm, .-pred_single_arm

// int pred_return_thumb(int x) { return x != 0 ? 7 : x + 1; }
    .thumb
    .globl pred_return_thumb
    .type pred_return_thumb, %function
    .thumb_func
pred_return_thumb:
    push {r4, r5, lr}
    mov r4, r0
    cmp r4, #0
    itt ne
    movne r0, #7
    popne.w {r4, r5, pc}
    adds r0, r4, #1
    pop {r4, r5, pc}
    .size pred_return_thumb, .-pred_return_thumb

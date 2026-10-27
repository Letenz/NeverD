.syntax unified
.arm
.text
.p2align 2

// exit(3): the number in r7, the status in r0.
.globl arm_linux_exit
.type arm_linux_exit, %function
arm_linux_exit:
  mov r0, #3
  mov r7, #1
  svc #0
  bx lr
.size arm_linux_exit, .-arm_linux_exit

// A syscall whose arity the table does not list, a number no kernel
// assigns, passes every argument register; here each one is set.
.globl arm_linux_seven
.type arm_linux_seven, %function
arm_linux_seven:
  push {r4, r5, r6, r7, lr}
  mov r0, #10
  mov r1, #11
  mov r2, #12
  mov r3, #13
  mov r4, #14
  mov r5, #15
  mov r6, #16
  movw r7, #0x5ab
  svc #0
  pop {r4, r5, r6, r7, pc}
.size arm_linux_seven, .-arm_linux_seven

// An OABI call, whose number is its immediate, has no modeled contract; the
// result is unused.
.globl arm_oabi_call
.type arm_oabi_call, %function
arm_oabi_call:
  svc #0x900014
  mov r0, #0
  bx lr
.size arm_oabi_call, .-arm_oabi_call

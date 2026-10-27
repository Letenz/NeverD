        .text
        .p2align 2

// exit_group(5): the number in x8, the status in x0.
        .globl a64_linux_exit_group
        .type a64_linux_exit_group, %function
a64_linux_exit_group:
        mov x0, #5
        mov x8, #94
        svc #0
        ret
        .size a64_linux_exit_group, .-a64_linux_exit_group

// A syscall whose arity the table does not list, a number no kernel
// assigns, passes every argument register; here each one is set.
        .globl a64_linux_six
        .type a64_linux_six, %function
a64_linux_six:
        mov x0, #20
        mov x1, #21
        mov x2, #22
        mov x3, #23
        mov x4, #24
        mov x5, #25
        mov x8, #0x5ab
        svc #0
        ret
        .size a64_linux_six, .-a64_linux_six

// Darwin's immediate enters no Linux kernel; the result is unused.
        .globl a64_other_immediate
        .type a64_other_immediate, %function
a64_other_immediate:
        svc #0x80
        mov x0, #0
        ret
        .size a64_other_immediate, .-a64_other_immediate

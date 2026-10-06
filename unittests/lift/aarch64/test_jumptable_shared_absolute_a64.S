// Three independent finite selectors sharing one absolute pointer object.
#ifndef TEST_CASE
#define TEST_CASE 0
#endif
.text
.p2align 2
.global shared_absolute
.type shared_absolute,%function
shared_absolute:
 sub sp,sp,#16
 adrp x8,shared_absolute_table
 add x8,x8,:lo12:shared_absolute_table
#if TEST_CASE == 1
 str x0,[x8,#32]
#elif TEST_CASE == 5
 str x8,[x3]
#endif
#if TEST_CASE == 3
 mov w9,w0
#else
 cmp w0,#0
 cset w9,eq
 orr w9,w9,#2
#endif
.Ldispatch:
 ldr x10,[x8,w9,uxtw #3]
 br x10
.global shared_absolute_case0
shared_absolute_case0:
 mov w0,#11
 b .Lreturn
.global shared_absolute_case1
shared_absolute_case1:
 mov w0,#29
 b .Lreturn
.global shared_absolute_case2
shared_absolute_case2:
#if TEST_CASE == 4
 mov x8,x3
#endif
 cmp w1,#0
 cset w9,eq
 orr w9,w9,#4
 ldr x10,[x8,w9,uxtw #3]
 br x10
.global shared_absolute_case3
shared_absolute_case3:
 cmp w2,#0
 cset w9,eq
 orr w9,w9,#6
 ldr x10,[x8,w9,uxtw #3]
 br x10
.global shared_absolute_case4
shared_absolute_case4:
#if TEST_CASE == 6
 mov w9,w3
 b .Ldispatch
#else
 mov w0,#19
 b .Lreturn
#endif
.global shared_absolute_case5
shared_absolute_case5:
 mov w0,#37
 b .Lreturn
.global shared_absolute_case6
shared_absolute_case6:
 mov w0,#53
 b .Lreturn
.global shared_absolute_case7
shared_absolute_case7:
 mov w0,#71
.Lreturn:
 add sp,sp,#16
 ret
.size shared_absolute,.-shared_absolute
#if TEST_CASE == 1
.section .data,"aw",%progbits
#else
.section .rodata,"a",%progbits
#endif
.p2align 3
.global shared_absolute_table
#if TEST_CASE != 2
.type shared_absolute_table,%object
#endif
shared_absolute_table:
 .quad shared_absolute_case0,shared_absolute_case1
 .quad shared_absolute_case2,shared_absolute_case3
 .quad shared_absolute_case4,shared_absolute_case5
 .quad shared_absolute_case6,shared_absolute_case7
#if TEST_CASE != 2
.size shared_absolute_table,.-shared_absolute_table
#endif
.section .note.GNU-stack,"",%progbits

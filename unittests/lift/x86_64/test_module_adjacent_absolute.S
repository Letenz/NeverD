#if defined(_WIN32) && !defined(__x86_64__)
#define adjacent_first _adjacent_first
#define adjacent_second _adjacent_second
#define adjacent_third _adjacent_third
#define adjacent_first_table _adjacent_first_table
#define adjacent_second_table _adjacent_second_table
#define adjacent_third_table _adjacent_third_table
#endif
// Two instruction-local, bounded absolute dispatches. The second table starts
// exactly one past the first. Encoded displacement width (4) and LOAD width
// (4 or 8) are deliberately distinct on x64.
#if defined(__x86_64__)
#define INDEX %rax
#define STRIDE 8
#define POINTER .quad
#define GET_INDEX movl %edi, %eax
#else
#define INDEX %eax
#define STRIDE 4
#define POINTER .long
#define GET_INDEX movl 4(%esp), %eax
#endif
#ifndef FLOAT_MODE
#define FLOAT_MODE 0
#endif
#ifndef THREE_TABLES
#define THREE_TABLES 0
#endif
#ifndef CFI_MODE
#define CFI_MODE 0
#endif
#ifndef DAMAGED_CFI
#define DAMAGED_CFI 0
#endif

        .text
        .globl adjacent_first
#if defined(_WIN32)
        .def adjacent_first; .scl 2; .type 32; .endef
#else
        .type adjacent_first,@function
#endif
adjacent_first:
#if CFI_MODE
        .cfi_startproc
#if DAMAGED_CFI == 3
        .cfi_signal_frame
#elif DAMAGED_CFI == 4
        .cfi_lsda 0x9b, adjacent_missing_lsda
#endif
#endif
        GET_INDEX
        andl $1, %eax
        jmp *adjacent_first_table(,INDEX,STRIDE)
.Lfirst_zero:
        movl $11, %eax
        ret
.Lfirst_one:
        movl $12, %eax
        ret
#if CFI_MODE
        .cfi_endproc
#endif
#if !defined(_WIN32)
        .size adjacent_first, .-adjacent_first
#endif

        .globl adjacent_second
#if defined(_WIN32)
        .def adjacent_second; .scl 2; .type 32; .endef
#else
        .type adjacent_second,@function
#endif
adjacent_second:
#if CFI_MODE
        .cfi_startproc
#if DAMAGED_CFI == 1
        .cfi_signal_frame
#elif DAMAGED_CFI == 2
        .cfi_lsda 0x9b, adjacent_missing_lsda
#endif
#endif
        GET_INDEX
        andl $1, %eax
        nop
        jmp *adjacent_second_table(,INDEX,STRIDE)
.Lsecond_zero:
#if FLOAT_MODE
        fld1
#if FLOAT_MODE == 2
        jmp .Lsecond_join
#else
        ret
#endif
#else
        movl $21, %eax
        ret
#endif
.Lsecond_one:
#if FLOAT_MODE == 1
        fldz
        ret
#elif FLOAT_MODE == 2
        xorl %eax, %eax
.Lsecond_join:
        fxam
        ret
#else
        movl $22, %eax
        ret
#endif
#if CFI_MODE
        .cfi_endproc
#endif
#if !defined(_WIN32)
        .size adjacent_second, .-adjacent_second
#endif

#if THREE_TABLES
        .globl adjacent_third
#if defined(_WIN32)
        .def adjacent_third; .scl 2; .type 32; .endef
#else
        .type adjacent_third,@function
#endif
adjacent_third:
#if CFI_MODE
        .cfi_startproc
#endif
        GET_INDEX
        andl $1, %eax
        jmp *adjacent_third_table(,INDEX,STRIDE)
.Lthird_zero:
        movl $31, %eax
        ret
.Lthird_one:
        movl $32, %eax
        ret
#if CFI_MODE
        .cfi_endproc
#endif
#if !defined(_WIN32)
        .size adjacent_third, .-adjacent_third
#endif
#endif

#if defined(_WIN32)
        .section .rdata,"dr"
#else
        .section .rodata.adjacent,"a",@progbits
#endif
        .p2align 3
        .globl adjacent_first_table
#if !defined(_WIN32)
        .type adjacent_first_table,@object
#endif
adjacent_first_table:
        POINTER .Lfirst_zero
        POINTER .Lfirst_one
#if !defined(_WIN32)
        .size adjacent_first_table, .-adjacent_first_table
#endif
        .globl adjacent_second_table
#if !defined(_WIN32)
        .type adjacent_second_table,@object
#endif
adjacent_second_table:
        POINTER .Lsecond_zero
        POINTER .Lsecond_one
#if !defined(_WIN32)
        .size adjacent_second_table, .-adjacent_second_table
#endif
#if THREE_TABLES
        .globl adjacent_third_table
#if !defined(_WIN32)
        .type adjacent_third_table,@object
#endif
adjacent_third_table:
        POINTER .Lthird_zero
        POINTER .Lthird_one
#if !defined(_WIN32)
        .size adjacent_third_table, .-adjacent_third_table
#endif
#endif
#if !defined(_WIN32)
        .section .note.GNU-stack,"",@progbits
#endif

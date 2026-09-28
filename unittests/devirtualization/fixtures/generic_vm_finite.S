// Original public VM with input-dependent finite bytecode addresses.
// ABI: uint64_t generic_vm_finite_*(uint64_t x, uint64_t y, uint64_t out[2]).
// r10 selects a readonly record; r9 is its correlated operand-decoding key.
// Every record stores a handler rel32, encoded operand, and continuation rel32.
// Several records share one native handler while retaining distinct VPCs.
// Context: x=0, y=8, accumulator=16, index=24, limit=32, status=40, out=48.

#if defined(_WIN32) || defined(GENERIC_VM_MS_ABI)
#define ARG_X %rcx
#define ARG_Y %rdx
#define ARG_OUT %r8
#else
#define ARG_X %rdi
#define ARG_Y %rsi
#define ARG_OUT %rdx
#endif
#if defined(_WIN32)
#define READONLY .section .rdata,"dr"
#else
#define READONLY .section .rodata,"a",@progbits
#endif

.macro finite_vm name, kind
.text
.p2align 4
.globl \name
#if defined(__ELF__)
.type \name, @function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  subq $64, %rsp
  movq ARG_OUT, 48(%rsp)
  movq ARG_X, 0(%rsp)
  movq ARG_Y, 8(%rsp)
  movq ARG_X, 16(%rsp)
  movq $0, 24(%rsp)
  movq $0, 40(%rsp)
  movq %rsp, %r11
  .if \kind == 0
    movq 0(%r11), %rax
    shrq $2, %rax
    andl $3, %eax
    movq %rax, 40(%r11)
    movl %eax, %r9d
    imull $17, %r9d
    addl $33, %r9d
    shlq $4, %rax
    leaq .Lf_records\@(%rip), %r10
    addq %rax, %r10
  .elseif \kind == 1
    movq 0(%r11), %rax
    andl $1, %eax
    movq %rax, 40(%r11)
    movl %eax, %r9d
    shll $4, %r9d
    addl $7, %r9d
    shlq $4, %rax
    leaq .Lf_records\@(%rip), %r10
    addq %rax, %r10
  .else
    movq 8(%r11), %rax
    andq $7, %rax
    movq %rax, 32(%r11)
    xorl %r9d, %r9d
    leaq .Lf_header_record\@(%rip), %r10
  .endif
.Lf_handler_base\@:
.Lf_dispatch\@:
  // This LOAD address is a finite expression, not a syntactic constant/ITE.
  movslq (%r10), %rax
  leaq .Lf_handler_base\@(%rip), %rcx
  addq %rcx, %rax
  addq $4, %r10
  jmpq *%rax

.globl \name\()_mixed_handler
\name\()_mixed_handler:
  movl (%r10), %eax
  xorl %r9d, %eax
  addq 0(%r11), %rax
  xorq 8(%r11), %rax
  movq %rax, 16(%r11)
  jmp .Lf_next\@

.Lf_add\@:
  movl (%r10), %eax
  xorl %r9d, %eax
  movq 0(%r11), %rdx
  addq 8(%r11), %rdx
  addq %rax, %rdx
  movq %rdx, 16(%r11)
  jmp .Lf_next\@

.Lf_sub\@:
  movl (%r10), %eax
  xorl %r9d, %eax
  movq 0(%r11), %rdx
  subq 8(%r11), %rdx
  addq %rax, %rdx
  movq %rdx, 16(%r11)
  jmp .Lf_next\@

.Lf_finalize\@:
  movl (%r10), %eax
  xorl %r9d, %eax
  xorq %rax, 16(%r11)
  jmp .Lf_next\@

.Lf_check\@:
  movq 24(%r11), %rax
  cmpq 32(%r11), %rax
  jae .Lf_exit\@
  // The record choice changes each iteration with live program state.
  xorq 16(%r11), %rax
  andl $1, %eax
  movl %eax, %r9d
  shll $4, %r9d
  addl $7, %r9d
  shlq $4, %rax
  leaq .Lf_records\@(%rip), %r10
  addq %rax, %r10
  jmp .Lf_dispatch\@

.globl \name\()_step_handler
\name\()_step_handler:
  movl (%r10), %eax
  xorl %r9d, %eax
  movq 16(%r11), %rdx
  addq 8(%r11), %rdx
  addq %rax, %rdx
  xorq 24(%r11), %rdx
  movq %rdx, 16(%r11)
  addq $1, 24(%r11)
  movq 24(%r11), %rdx
  movq %rdx, 40(%r11)
  jmp .Lf_next\@

.Lf_next\@:
  movslq 4(%r10), %rcx
  leaq \name\()_bytecode(%rip), %r10
  addq %rcx, %r10
  xorl %r9d, %r9d
  jmp .Lf_dispatch\@

.Lf_exit\@:
  leaq .Lf_return_record\@(%rip), %r10
  xorl %r9d, %r9d
  jmp .Lf_dispatch\@

.Lf_return\@:
  movq 16(%r11), %rax
  movq 48(%r11), %rcx
  movq %rax, 0(%rcx)
  movq 40(%r11), %rdx
  movq %rdx, 8(%rcx)
  addq $64, %rsp
  retq
#if defined(__ELF__)
.size \name, .-\name
#endif

READONLY
.p2align 4
.globl \name\()_bytecode
\name\()_bytecode:
.Lf_records\@:
  .if \kind == 0
    .long \name\()_mixed_handler - .Lf_handler_base\@, 13 ^ 33
    .long .Lf_return_record\@ - \name\()_bytecode, 0
    .long \name\()_mixed_handler - .Lf_handler_base\@, 29 ^ 50
    .long .Lf_return_record\@ - \name\()_bytecode, 0
    .long \name\()_mixed_handler - .Lf_handler_base\@, 47 ^ 67
    .long .Lf_return_record\@ - \name\()_bytecode, 0
    .long \name\()_mixed_handler - .Lf_handler_base\@, 71 ^ 84
    .long .Lf_return_record\@ - \name\()_bytecode, 0
  .elseif \kind == 1
    .long .Lf_sub\@ - .Lf_handler_base\@, 5 ^ 7
    .long .Lf_join_record\@ - \name\()_bytecode, 0
    .long .Lf_add\@ - .Lf_handler_base\@, 9 ^ 23
    .long .Lf_join_record\@ - \name\()_bytecode, 0
.Lf_join_record\@:
    .long .Lf_finalize\@ - .Lf_handler_base\@, 0x2d
    .long .Lf_return_record\@ - \name\()_bytecode, 0
  .else
    .long \name\()_step_handler - .Lf_handler_base\@, 5 ^ 7
    .long .Lf_header_record\@ - \name\()_bytecode, 0
    .long \name\()_step_handler - .Lf_handler_base\@, 11 ^ 23
    .long .Lf_header_record\@ - \name\()_bytecode, 0
.Lf_header_record\@:
    .long .Lf_check\@ - .Lf_handler_base\@
  .endif
.Lf_return_record\@:
  .long .Lf_return\@ - .Lf_handler_base\@
.globl \name\()_bytecode_end
\name\()_bytecode_end:
.endm

finite_vm generic_vm_finite_arithmetic, 0
finite_vm generic_vm_finite_branch, 1
finite_vm generic_vm_finite_loop, 2

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

// Original, public test VM: register operands and direct-threaded dispatch.
// ABI: uint64_t generic_vm_register_*(uint64_t x, uint64_t y, uint64_t out[2]).
// r10 is the bytecode cursor, r11 the private stack context. Signed bytecode offsets
// name native handlers; each handler fetches the next offset independently.
// Register operands are context byte offsets XOR 0xa5. Immediates XOR 0x31415926.
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

.macro threaded_next
  movslq (%r10), %rax
  addq %r10, %rax
  addq $4, %r10
  jmpq *%rax
.endm

.macro register_vm name, kind
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
  movq $0, 40(%rsp)
  movq %rsp, %r11
  leaq \name\()_bytecode(%rip), %r10
  threaded_next

.Lr_add\@:
  movzbl 0(%r10), %edx
  xorl $0xa5, %edx
  movzbl 1(%r10), %ecx
  xorl $0xa5, %ecx
  movq (%r11,%rcx), %rax
  movzbl 2(%r10), %ecx
  xorl $0xa5, %ecx
  addq (%r11,%rcx), %rax
  setb 40(%r11)
  movq %rax, (%r11,%rdx)
  addq $3, %r10
  threaded_next

.Lr_sub\@:
  movzbl 0(%r10), %edx
  xorl $0xa5, %edx
  movzbl 1(%r10), %ecx
  xorl $0xa5, %ecx
  movq (%r11,%rcx), %rax
  movzbl 2(%r10), %ecx
  xorl $0xa5, %ecx
  subq (%r11,%rcx), %rax
  setb 40(%r11)
  movq %rax, (%r11,%rdx)
  addq $3, %r10
  threaded_next

.Lr_xor\@:
  movzbl (%r10), %edx
  xorl $0xa5, %edx
  movl 1(%r10), %eax
  xorl $0x31415926, %eax
  xorq %rax, (%r11,%rdx)
  addq $5, %r10
  threaded_next

.Lr_odd\@:
  movzbl (%r10), %edx
  xorl $0xa5, %edx
  movslq 1(%r10), %rcx
  addq $5, %r10
  testq $1, (%r11,%rdx)
  jz .Lr_even\@
  addq %rcx, %r10
.Lr_even\@:
  threaded_next

.Lr_jump\@:
  movslq (%r10), %rcx
  addq $4, %r10
  addq %rcx, %r10
  threaded_next

.Lr_init\@:
  movq 0(%r11), %rax
  movq %rax, 16(%r11)
  movq 8(%r11), %rax
  andq $7, %rax
  movq %rax, 32(%r11)
  movq $0, 24(%r11)
  threaded_next

.Lr_done\@:
  movslq (%r10), %rcx
  addq $4, %r10
  movq 24(%r11), %rax
  cmpq 32(%r11), %rax
  jb .Lr_continue\@
  addq %rcx, %r10
.Lr_continue\@:
  threaded_next

// A combined handler with three live effects, including loop-carried state.
.Lr_step\@:
  movq 16(%r11), %rax
  addq 8(%r11), %rax
  xorq 24(%r11), %rax
  movq %rax, 16(%r11)
  addq $1, 24(%r11)
  movq 24(%r11), %rax
  movq %rax, 40(%r11)
  threaded_next

.Lr_return\@:
  movzbl (%r10), %edx
  xorl $0xa5, %edx
  movq (%r11,%rdx), %rax
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
.p2align 3
.globl \name\()_bytecode
\name\()_bytecode:
  .if \kind == 0
    .long .Lr_add\@ - .
    .byte 16 ^ 0xa5, 0 ^ 0xa5, 8 ^ 0xa5
    .long .Lr_xor\@ - .
    .byte 16 ^ 0xa5
    .long 0x13579bdf ^ 0x31415926
  .elseif \kind == 1
    .long .Lr_odd\@ - .
    .byte 0 ^ 0xa5
    .long .Lr_bc_odd\@ - . - 4
    .long .Lr_sub\@ - .
    .byte 16 ^ 0xa5, 0 ^ 0xa5, 8 ^ 0xa5
    .long .Lr_jump\@ - .
    .long .Lr_bc_join\@ - . - 4
.Lr_bc_odd\@:
    .long .Lr_add\@ - .
    .byte 16 ^ 0xa5, 0 ^ 0xa5, 8 ^ 0xa5
.Lr_bc_join\@:
    .long .Lr_xor\@ - .
    .byte 16 ^ 0xa5
    .long 0x13579bdf ^ 0x31415926
  .else
    .long .Lr_init\@ - .
.Lr_bc_header\@:
    .long .Lr_done\@ - .
    .long .Lr_bc_exit\@ - . - 4
    .long .Lr_step\@ - .
    .long .Lr_jump\@ - .
    .long .Lr_bc_header\@ - . - 4
.Lr_bc_exit\@:
  .endif
  .long .Lr_return\@ - .
  .byte 16 ^ 0xa5
.globl \name\()_bytecode_end
\name\()_bytecode_end:
.endm

register_vm generic_vm_register_arithmetic, 0
register_vm generic_vm_register_branch, 1
register_vm generic_vm_register_loop, 2

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

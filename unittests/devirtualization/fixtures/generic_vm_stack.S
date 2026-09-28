// Original, public test VM: operand stack and one opcode-table dispatcher.
// ABI and observable results match generic_vm_register.S. r10 is the bytecode
// cursor, r11 the private context, r9 the first free operand-stack cell.
// Opcodes XOR 0x5a, local offsets XOR 0xa5, immediates XOR 0x31415926.
// Context: x=0, y=8, limit=16, index=24, status=32, out=40; values start at 64.

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

.macro stack_vm name, kind
.text
.p2align 4
.globl \name
#if defined(__ELF__)
.type \name, @function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  subq $192, %rsp
  movq ARG_OUT, 40(%rsp)
  movq ARG_X, 0(%rsp)
  movq ARG_Y, 8(%rsp)
  movq $0, 32(%rsp)
  movq %rsp, %r11
  leaq 64(%rsp), %r9
  leaq \name\()_bytecode(%rip), %r10
.Ls_dispatch\@:
  movzbl (%r10), %eax
  incq %r10
  xorl $0x5a, %eax
  cmpl $9, %eax
  ja .Ls_invalid\@
  leaq .Ls_handlers\@(%rip), %rcx
  movslq (%rcx,%rax,4), %rax
  addq %rcx, %rax
  jmpq *%rax

.Ls_push\@:
  movzbl (%r10), %edx
  xorl $0xa5, %edx
  incq %r10
  movq (%r11,%rdx), %rax
  movq %rax, (%r9)
  addq $8, %r9
  jmp .Ls_dispatch\@

.Ls_add\@:
  subq $8, %r9
  movq (%r9), %rax
  addq %rax, -8(%r9)
  setb 32(%r11)
  jmp .Ls_dispatch\@

.Ls_sub\@:
  subq $8, %r9
  movq (%r9), %rax
  subq %rax, -8(%r9)
  setb 32(%r11)
  jmp .Ls_dispatch\@

.Ls_xor\@:
  movl (%r10), %eax
  xorl $0x31415926, %eax
  addq $4, %r10
  xorq %rax, -8(%r9)
  jmp .Ls_dispatch\@

.Ls_odd\@:
  subq $8, %r9
  movslq (%r10), %rcx
  addq $4, %r10
  testq $1, (%r9)
  jz .Ls_dispatch\@
  addq %rcx, %r10
  jmp .Ls_dispatch\@

.Ls_jump\@:
  movslq (%r10), %rcx
  addq $4, %r10
  addq %rcx, %r10
  jmp .Ls_dispatch\@

.Ls_init\@:
  movq 8(%r11), %rax
  andq $7, %rax
  movq %rax, 16(%r11)
  movq $0, 24(%r11)
  jmp .Ls_dispatch\@

.Ls_done\@:
  movslq (%r10), %rcx
  addq $4, %r10
  movq 24(%r11), %rax
  cmpq 16(%r11), %rax
  jb .Ls_dispatch\@
  addq %rcx, %r10
  jmp .Ls_dispatch\@

// Combined stack operation plus loop-counter/status effects.
.Ls_step\@:
  movq -8(%r9), %rax
  addq 8(%r11), %rax
  xorq 24(%r11), %rax
  movq %rax, -8(%r9)
  addq $1, 24(%r11)
  movq 24(%r11), %rax
  movq %rax, 32(%r11)
  jmp .Ls_dispatch\@

.Ls_return\@:
  movq -8(%r9), %rax
  movq 40(%r11), %rcx
  movq %rax, 0(%rcx)
  movq 32(%r11), %rdx
  movq %rdx, 8(%rcx)
  addq $192, %rsp
  retq
.Ls_invalid\@:
  ud2
#if defined(__ELF__)
.size \name, .-\name
#endif

READONLY
.p2align 3
.Ls_handlers\@:
  .long .Ls_push\@ - .Ls_handlers\@
  .long .Ls_add\@ - .Ls_handlers\@
  .long .Ls_sub\@ - .Ls_handlers\@
  .long .Ls_xor\@ - .Ls_handlers\@
  .long .Ls_odd\@ - .Ls_handlers\@
  .long .Ls_jump\@ - .Ls_handlers\@
  .long .Ls_init\@ - .Ls_handlers\@
  .long .Ls_done\@ - .Ls_handlers\@
  .long .Ls_step\@ - .Ls_handlers\@
  .long .Ls_return\@ - .Ls_handlers\@
.globl \name\()_bytecode
\name\()_bytecode:
  .if \kind == 0
    .byte 0 ^ 0x5a, 0 ^ 0xa5, 0 ^ 0x5a, 8 ^ 0xa5, 1 ^ 0x5a
    .byte 3 ^ 0x5a
    .long 0x13579bdf ^ 0x31415926
  .elseif \kind == 1
    .byte 0 ^ 0x5a, 0 ^ 0xa5, 4 ^ 0x5a
    .long .Ls_bc_odd\@ - . - 4
    .byte 0 ^ 0x5a, 0 ^ 0xa5, 0 ^ 0x5a, 8 ^ 0xa5, 2 ^ 0x5a
    .byte 5 ^ 0x5a
    .long .Ls_bc_join\@ - . - 4
.Ls_bc_odd\@:
    .byte 0 ^ 0x5a, 0 ^ 0xa5, 0 ^ 0x5a, 8 ^ 0xa5, 1 ^ 0x5a
.Ls_bc_join\@:
    .byte 3 ^ 0x5a
    .long 0x13579bdf ^ 0x31415926
  .else
    .byte 0 ^ 0x5a, 0 ^ 0xa5, 6 ^ 0x5a
.Ls_bc_header\@:
    .byte 7 ^ 0x5a
    .long .Ls_bc_exit\@ - . - 4
    .byte 8 ^ 0x5a, 5 ^ 0x5a
    .long .Ls_bc_header\@ - . - 4
.Ls_bc_exit\@:
  .endif
  .byte 9 ^ 0x5a
.globl \name\()_bytecode_end
\name\()_bytecode_end:
.endm

stack_vm generic_vm_stack_arithmetic, 0
stack_vm generic_vm_stack_branch, 1
stack_vm generic_vm_stack_loop, 2

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

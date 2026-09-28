// Original public example of dispatch state carried through a stack bank.
// ABI: uint64_t generic_control_state(uint64_t input).
// The selector crosses two native branches: first in memory, then in r10.

#if defined(_WIN32)
#define INPUT %rcx
#else
#define INPUT %rdi
#endif

.text
.globl generic_control_state
#if defined(__ELF__)
.type generic_control_state,@function
#endif
generic_control_state:
  subq $40, %rsp
  movq INPUT, %rax
  xorq $85, %rax
  movq %rax, 0(%rsp)
  movq INPUT, %rax
  andq $3, %rax
  movq %rax, 16(%rsp)
  jmp .Lselector_reload
.Lselector_reload:
  movq 16(%rsp), %r10
  jmp .Lselector_dispatch
.Lselector_dispatch:
  leaq .Lselector_table(%rip), %r11
  movq (%r11,%r10,8), %rax
  jmp *%rax
.Lselector_zero:
  movq 0(%rsp), %rax
  addq $9, %rax
  jmp .Lselector_return
.Lselector_one:
  movq 0(%rsp), %rax
  addq $22, %rax
  jmp .Lselector_return
.Lselector_two:
  movq 0(%rsp), %rax
  addq $35, %rax
  jmp .Lselector_return
.Lselector_three:
  movq 0(%rsp), %rax
  addq $48, %rax
.Lselector_return:
  addq $40, %rsp
  ret
#if defined(__ELF__)
.size generic_control_state, .-generic_control_state
.section .rodata
#else
.section .rdata,"dr"
#endif
.p2align 3
.Lselector_table:
  .quad .Lselector_zero, .Lselector_one, .Lselector_two, .Lselector_three

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

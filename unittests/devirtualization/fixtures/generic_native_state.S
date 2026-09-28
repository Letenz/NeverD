.text
.globl generic_state_flags
.type generic_state_flags,@function
generic_state_flags:
  pushfq
  popq %rax
  ret
.size generic_state_flags, .-generic_state_flags

.globl generic_state_call
.type generic_state_call,@function
generic_state_call:
  pushq %rbx
  movq %rdi, %rbx
  call .Ltriple
  popq %rbx
  ret
.Ltriple:
  leaq 19(%rbx,%rbx,2), %rax
  ret
.size generic_state_call, .-generic_state_call

.globl generic_state_spill
.type generic_state_spill,@function
generic_state_spill:
  movq %rsp, %r11
  subq $24, %rsp
  movq %r11, 8(%rsp)
  movq $37, %rax
  movq 8(%rsp), %rsp
  ret
.size generic_state_spill, .-generic_state_spill

.globl generic_state_rdssp
.type generic_state_rdssp,@function
generic_state_rdssp:
  movq $123, %rax
  rdsspq %rax
  ret
.size generic_state_rdssp, .-generic_state_rdssp

.globl generic_state_incssp
.type generic_state_incssp,@function
generic_state_incssp:
  incsspq %rax
  ret
.size generic_state_incssp, .-generic_state_incssp
.section .note.GNU-stack,"",@progbits

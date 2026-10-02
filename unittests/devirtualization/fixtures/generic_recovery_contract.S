// Independently authored arithmetic/control fixtures for public recovery options.
.text
.globl generic_recovery_chain
.type generic_recovery_chain,@function
generic_recovery_chain:
  movq %rdi, %r10
  xorq $90, %r10
  jmp .Lchain_middle
.Lchain_middle:
  xorq %rdi, %r10
  leaq .Lchain_exit(%rip), %r11
  subq $90, %r11
  addq %r10, %r11
  jmp *%r11
.Lchain_exit:
  leaq 17(%rdi), %rax
  ret
.size generic_recovery_chain,.-generic_recovery_chain

.globl generic_recovery_bounds
.type generic_recovery_bounds,@function
generic_recovery_bounds:
  leaq -16(%rsp), %r10
  leaq -48(%rsp), %r11
  subq $80, %rsp
  cmpq %r10, %r11
  jb .Lbounds_exit
  ud2
.Lbounds_exit:
  addq $80, %rsp
  leaq 17(%rdi), %rax
  ret
.size generic_recovery_bounds,.-generic_recovery_bounds
.section .note.GNU-stack,"",@progbits

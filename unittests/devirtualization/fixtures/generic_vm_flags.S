// Public flag-state fixture: a bounded indirect dispatch still reads the
// machine flag image and restores it at runtime.

.text
.p2align 4
.globl generic_vm_flags
.type generic_vm_flags, @function
generic_vm_flags:
  // Define every arithmetic flag before capturing the full image. Source C
  // has no contract for arbitrary incoming caller flags.
  movq $0, %rax
  addq $0, %rax
  cld
  pushfq
  popq %rax
  pushq %rax
  popfq
  andq $1, %rax
  leaq .Lflags_targets(%rip), %r10
  movslq (%r10,%rax,4), %r11
  addq %r10, %r11
  jmpq *%r11

.Lflags_zero:
  leaq 42(%rdi), %rax
  retq

.Lflags_one:
  leaq 99(%rdi), %rax
  retq
.size generic_vm_flags, .-generic_vm_flags

.section .rodata,"a",@progbits
.p2align 2
.Lflags_targets:
  .long .Lflags_zero-.Lflags_targets
  .long .Lflags_one-.Lflags_targets

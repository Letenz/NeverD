// Original public memory-indirect near CALL fixtures. The observer supplies
// explicit guest registers, flags and stack; these are not C ABI functions.
.text
.p2align 4
.globl generic_memory_rip_call
.type generic_memory_rip_call,@function
generic_memory_rip_call:
  callq *generic_memory_readonly_slot(%rip)
.globl generic_memory_rip_continuation
generic_memory_rip_continuation:
  movq %rsp, %r11
  retq
.size generic_memory_rip_call, .-generic_memory_rip_call

.p2align 4
.globl generic_memory_table_call
.type generic_memory_table_call,@function
generic_memory_table_call:
  pushfq
  popq %r8
  movq %rdi, %rdx
  andq $1, %rdx
  leaq generic_memory_readonly_table(%rip), %rcx
  pushq %r8
  popfq
  callq *(%rcx,%rdx,8)
.globl generic_memory_table_continuation
generic_memory_table_continuation:
  movq %rsp, %r11
  retq
.size generic_memory_table_call, .-generic_memory_table_call

.p2align 4
.globl generic_memory_stack_call
.type generic_memory_stack_call,@function
generic_memory_stack_call:
  pushfq
  popq %r8
  testq $1, %rdi
  leaq generic_memory_callee_zero(%rip), %rdx
  leaq generic_memory_callee_one(%rip), %rcx
  cmovneq %rcx, %rdx
  pushq %r8
  popfq
  leaq -16(%rsp), %rsp
  movq %rdx, (%rsp)
  callq *(%rsp)
.globl generic_memory_stack_continuation
generic_memory_stack_continuation:
  leaq 16(%rsp), %rsp
  movq %rsp, %r11
  retq
.size generic_memory_stack_call, .-generic_memory_stack_call

.p2align 4
.globl generic_memory_overwritten_slot_call
.type generic_memory_overwritten_slot_call,@function
generic_memory_overwritten_slot_call:
  leaq generic_memory_callee_one(%rip), %rdx
  movq %rdx, -8(%rsp)
  // The target is read before CALL overwrites this same slot with its return.
  callq *-8(%rsp)
.globl generic_memory_overwritten_continuation
generic_memory_overwritten_continuation:
  movq %rsp, %r11
  retq
.size generic_memory_overwritten_slot_call, .-generic_memory_overwritten_slot_call

.globl generic_memory_callee_zero
.type generic_memory_callee_zero,@function
generic_memory_callee_zero:
  movq %rax, %rbx
  movq (%rsp), %r9
  movq %rsp, %r10
  leaq 51(%rdi,%rsi,2), %rax
  retq
.size generic_memory_callee_zero, .-generic_memory_callee_zero

.globl generic_memory_callee_one
.type generic_memory_callee_one,@function
generic_memory_callee_one:
  movq %rax, %rbx
  movq (%rsp), %r9
  movq %rsp, %r10
  leaq 73(%rdi,%rsi,4), %rax
  retq
.size generic_memory_callee_one, .-generic_memory_callee_one

.globl generic_memory_external_call
.type generic_memory_external_call,@function
generic_memory_external_call:
  callq *(%rdi)
  retq
.size generic_memory_external_call, .-generic_memory_external_call

.globl generic_memory_writable_call
.type generic_memory_writable_call,@function
generic_memory_writable_call:
  callq *generic_memory_writable_slot(%rip)
  retq
.size generic_memory_writable_call, .-generic_memory_writable_call

.globl generic_memory_unknown_stack_call
.type generic_memory_unknown_stack_call,@function
generic_memory_unknown_stack_call:
  movq %rdi, -8(%rsp)
  callq *-8(%rsp)
  retq
.size generic_memory_unknown_stack_call, .-generic_memory_unknown_stack_call

.globl generic_memory_alias_clobbered_call
.type generic_memory_alias_clobbered_call,@function
generic_memory_alias_clobbered_call:
  leaq generic_memory_callee_zero(%rip), %rdx
  movq %rdx, -8(%rsp)
  // An external store may overwrite the target slot without touching the
  // entry return slot. Its contents cannot borrow an immutable-read proof.
  movq %rsi, (%rdi)
  callq *-8(%rsp)
  retq
.size generic_memory_alias_clobbered_call, .-generic_memory_alias_clobbered_call

.macro invalid_table_call name, table
.globl \name
.type \name,@function
\name:
  movq %rdi, %rdx
  andq $1, %rdx
  leaq \table(%rip), %rcx
  callq *(%rcx,%rdx,8)
  retq
.size \name, .-\name
.endm
invalid_table_call generic_memory_missing_target_call, generic_memory_missing_table
invalid_table_call generic_memory_nonexec_target_call, generic_memory_nonexec_table

.section .rodata,"a",@progbits
.p2align 3
.globl generic_memory_readonly_slot
generic_memory_readonly_slot:
  .quad generic_memory_callee_zero
.globl generic_memory_readonly_table
generic_memory_readonly_table:
  .quad generic_memory_callee_zero, generic_memory_callee_one
generic_memory_missing_table:
  .quad generic_memory_callee_zero, 0
generic_memory_nonexec_table:
  .quad generic_memory_callee_zero, generic_memory_readonly_slot
.data
.p2align 3
generic_memory_writable_slot:
  .quad generic_memory_callee_zero
.section .note.GNU-stack,"",@progbits

// Original public negative inputs. These functions are analysis fixtures;
// their purpose is to ensure unresolved control is never replaced by a guess.
#if defined(_WIN32)
#define ARG_X %rcx
#define READONLY .section .rdata,"dr"
#else
#define ARG_X %rdi
#define READONLY .section .rodata,"a",@progbits
#endif

.text
.globl generic_vm_unresolved_dispatch
#if defined(__ELF__)
.type generic_vm_unresolved_dispatch, @function
#elif defined(_WIN32)
.def generic_vm_unresolved_dispatch; .scl 2; .type 32; .endef
#endif
generic_vm_unresolved_dispatch:
  leaq generic_vm_unresolved_dispatch_bytecode(%rip), %r10
  movslq (%r10), %rax
  addq %r10, %rax
  // An unconstrained program input contributes to a native control target.
  addq ARG_X, %rax
  jmpq *%rax
.Lunresolved_return:
  movq $37, %rax
  retq
#if defined(__ELF__)
.size generic_vm_unresolved_dispatch, .-generic_vm_unresolved_dispatch
#endif

.globl generic_vm_writable_dispatch
#if defined(__ELF__)
.type generic_vm_writable_dispatch, @function
#elif defined(_WIN32)
.def generic_vm_writable_dispatch; .scl 2; .type 32; .endef
#endif
generic_vm_writable_dispatch:
  leaq generic_vm_writable_dispatch_bytecode(%rip), %r10
  // File bytes do not establish the value of a writable runtime location.
  movslq (%r10), %rax
  addq %r10, %rax
  jmpq *%rax
.Lwritable_return:
  movq $41, %rax
  retq
#if defined(__ELF__)
.size generic_vm_writable_dispatch, .-generic_vm_writable_dispatch
#endif

.globl generic_vm_return_dispatch
#if defined(__ELF__)
.type generic_vm_return_dispatch, @function
#elif defined(_WIN32)
.def generic_vm_return_dispatch; .scl 2; .type 32; .endef
#endif
generic_vm_return_dispatch:
  // RET can be an interpreter dispatch, not a source function return.
  leaq .Lreturn_dispatch_target(%rip), %rax
  pushq %rax
  retq
.Lreturn_dispatch_target:
  movq $43, %rax
  retq
#if defined(__ELF__)
.size generic_vm_return_dispatch, .-generic_vm_return_dispatch
#endif

.globl generic_vm_callee_pop
#if defined(__ELF__)
.type generic_vm_callee_pop, @function
#elif defined(_WIN32)
.def generic_vm_callee_pop; .scl 2; .type 32; .endef
#endif
generic_vm_callee_pop:
  // The ordinary source ABI cannot express this extra caller-stack change.
  movq $47, %rax
  retq $8
#if defined(__ELF__)
.size generic_vm_callee_pop, .-generic_vm_callee_pop
#endif

// These system/far transfers must not be collapsed into an ordinary C return.
.globl generic_vm_far_return
#if defined(__ELF__)
.type generic_vm_far_return, @function
#elif defined(_WIN32)
.def generic_vm_far_return; .scl 2; .type 32; .endef
#endif
generic_vm_far_return:
  .byte 0x48, 0xcb // lretq
#if defined(__ELF__)
.size generic_vm_far_return, .-generic_vm_far_return
#endif

.globl generic_vm_interrupt_return
#if defined(__ELF__)
.type generic_vm_interrupt_return, @function
#elif defined(_WIN32)
.def generic_vm_interrupt_return; .scl 2; .type 32; .endef
#endif
generic_vm_interrupt_return:
  .byte 0x48, 0xcf // iretq
#if defined(__ELF__)
.size generic_vm_interrupt_return, .-generic_vm_interrupt_return
#endif

.globl generic_vm_system_return
#if defined(__ELF__)
.type generic_vm_system_return, @function
#elif defined(_WIN32)
.def generic_vm_system_return; .scl 2; .type 32; .endef
#endif
generic_vm_system_return:
  .byte 0x0f, 0xaa // rsm
#if defined(__ELF__)
.size generic_vm_system_return, .-generic_vm_system_return
#endif

READONLY
.globl generic_vm_unresolved_dispatch_bytecode
generic_vm_unresolved_dispatch_bytecode:
  .long .Lunresolved_return - .
.data
.globl generic_vm_writable_dispatch_bytecode
generic_vm_writable_dispatch_bytecode:
  .long .Lwritable_return - .

#if defined(__ELF__)
.section .note.GNU-stack,"",@progbits
#endif

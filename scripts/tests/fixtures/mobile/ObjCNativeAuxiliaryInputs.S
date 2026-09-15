#if defined(__aarch64__) || defined(__arm64__)
.text
.p2align 2
.globl _nd_auxiliary_fill_bridge
.private_extern _nd_auxiliary_fill_bridge
_nd_auxiliary_fill_bridge:
  stp x29, x30, [sp, #-16]!
  mov x8, x1
  bl _nd_auxiliary_fill
  ldp x29, x30, [sp], #16
  ret
.p2align 2
.globl _nd_auxiliary_fill
.private_extern _nd_auxiliary_fill
_nd_auxiliary_fill:
  str x0, [x8]
  mvn x9, x0
  str x9, [x8, #8]
  add x10, x0, #7
  str x10, [x8, #16]
  eor x0, x9, x10
  mov x8, #85
  ret
.p2align 2
.globl _nd_auxiliary_add_bridge
.private_extern _nd_auxiliary_add_bridge
_nd_auxiliary_add_bridge:
  stp x29, x30, [sp, #-16]!
  mov x8, x1
  bl _nd_auxiliary_add
  ldp x29, x30, [sp], #16
  ret
.p2align 2
.globl _nd_auxiliary_add
.private_extern _nd_auxiliary_add
_nd_auxiliary_add:
  ldr x9, [x8]
  add x9, x0, x9
  str x9, [x8]
  ldr x10, [x8, #8]
  eor x0, x9, x10
  mov x8, #170
  ret
#elif defined(__x86_64__)
.text
.p2align 4
.globl _nd_auxiliary_fill_bridge
.private_extern _nd_auxiliary_fill_bridge
_nd_auxiliary_fill_bridge:
  subq $8, %rsp
  movq %rsi, %r10
  callq _nd_auxiliary_fill
  addq $8, %rsp
  ret
.p2align 4
.globl _nd_auxiliary_fill
.private_extern _nd_auxiliary_fill
_nd_auxiliary_fill:
  movq %rdi, (%r10)
  movq %rdi, %rax
  notq %rax
  movq %rax, 8(%r10)
  leaq 7(%rdi), %rcx
  movq %rcx, 16(%r10)
  xorq %rcx, %rax
  movq $85, %r10
  ret
.p2align 4
.globl _nd_auxiliary_add_bridge
.private_extern _nd_auxiliary_add_bridge
_nd_auxiliary_add_bridge:
  subq $8, %rsp
  movq %rsi, %r10
  callq _nd_auxiliary_add
  addq $8, %rsp
  ret
.p2align 4
.globl _nd_auxiliary_add
.private_extern _nd_auxiliary_add
_nd_auxiliary_add:
  movq (%r10), %rax
  addq %rdi, %rax
  movq %rax, (%r10)
  xorq 8(%r10), %rax
  movq $170, %r10
  ret
#else
#error Unsupported fixture architecture
#endif

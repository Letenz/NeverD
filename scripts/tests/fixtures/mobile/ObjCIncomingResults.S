#if defined(__aarch64__) || defined(__arm64__)
.text
.p2align 2
.globl _nd_conditional_word
.private_extern _nd_conditional_word
_nd_conditional_word:
  tbz x1, #0, 1f
  add x0, x0, x1
1:
  ret
.p2align 2
.globl _nd_conditional_memory
.private_extern _nd_conditional_memory
_nd_conditional_memory:
  ldr x8, [x1]
  tbz x8, #0, 1f
  add x0, x0, x8
1:
  ret
#elif defined(__x86_64__)
.text
.p2align 4
.globl _nd_conditional_word
.private_extern _nd_conditional_word
_nd_conditional_word:
  movq %rdi, %rax
  testb $1, %sil
  jz 1f
  addq %rsi, %rax
1:
  ret
.p2align 4
.globl _nd_conditional_memory
.private_extern _nd_conditional_memory
_nd_conditional_memory:
  movq %rdi, %rax
  movq (%rsi), %rdx
  testb $1, %dl
  jz 1f
  addq %rdx, %rax
1:
  ret
#else
#error Unsupported fixture architecture
#endif

// Keep the result computation in predecessors of a shared machine return.
// The compiler-generated Objective-C caller exercises the inferred native ABI.
.text
.globl _NDPathResult
.private_extern _NDPathResult
.p2align 2
_NDPathResult:
#if defined(__aarch64__)
  cbz w1, Lzero
  add x0, x0, #7
  b Ljoin
Lzero:
  sub x0, x0, #9
Ljoin:
  str x0, [x2]
  ret
#elif defined(__x86_64__)
  testl %esi, %esi
  jz Lzero
  leaq 7(%rdi), %rax
  jmp Ljoin
Lzero:
  leaq -9(%rdi), %rax
Ljoin:
  movq %rax, (%rdx)
  retq
#else
#error Unsupported fixture architecture
#endif

// Narrow instruction results still define the complete architectural return
// register. The caller observes all 64 bits, including zero upper bits.
.globl _NDWideLeaf
.private_extern _NDWideLeaf
.p2align 2
_NDWideLeaf:
#if defined(__aarch64__)
  cmp x0, #21
  b.lt LwideSmall
  mov w0, #7
  ret
LwideSmall:
  mov w0, #-1
  ret
#elif defined(__x86_64__)
  cmpq $21, %rdi
  jl LwideSmall
  movl $7, %eax
  retq
LwideSmall:
  movl $-1, %eax
  retq
#endif

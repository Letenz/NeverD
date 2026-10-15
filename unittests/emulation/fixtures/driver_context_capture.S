//===- driver_context_capture.S - Independent caller-state fixture -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Fixed-frame Win64 wrapper, including unwindable flag sampling windows.
.text
.p2align 4
.globl CaptureDriverContext
.seh_proc CaptureDriverContext
CaptureDriverContext:
  pushq %rbx
  .seh_pushreg %rbx
  pushq %rbp
  .seh_pushreg %rbp
  pushq %rsi
  .seh_pushreg %rsi
  pushq %rdi
  .seh_pushreg %rdi
  pushq %r12
  .seh_pushreg %r12
  pushq %r13
  .seh_pushreg %r13
  pushq %r14
  .seh_pushreg %r14
  pushq %r15
  .seh_pushreg %r15
  subq $360, %rsp
  .seh_stackalloc 360
  leaq 128(%rsp), %rbp
  .seh_setframe %rbp, 128
  movdqu %xmm0, 64(%rsp)
  movdqu %xmm1, 80(%rsp)
  movdqu %xmm2, 96(%rsp)
  movdqu %xmm3, 112(%rsp)
  movdqu %xmm4, 128(%rsp)
  movdqu %xmm5, 144(%rsp)
  movdqu %xmm6, 160(%rsp)
  .seh_savexmm %xmm6, 160
  movdqu %xmm7, 176(%rsp)
  .seh_savexmm %xmm7, 176
  movdqu %xmm8, 192(%rsp)
  .seh_savexmm %xmm8, 192
  movdqu %xmm9, 208(%rsp)
  .seh_savexmm %xmm9, 208
  movdqu %xmm10, 224(%rsp)
  .seh_savexmm %xmm10, 224
  movdqu %xmm11, 240(%rsp)
  .seh_savexmm %xmm11, 240
  movdqu %xmm12, 256(%rsp)
  .seh_savexmm %xmm12, 256
  movdqu %xmm13, 272(%rsp)
  .seh_savexmm %xmm13, 272
  movdqu %xmm14, 288(%rsp)
  .seh_savexmm %xmm14, 288
  movdqu %xmm15, 304(%rsp)
  .seh_savexmm %xmm15, 304
  .seh_endprologue
  movq %rcx, 32(%rsp)
  movq %rdx, 40(%rsp)
  movq %r8, 48(%rsp)
  movl %r9d, 56(%rsp)
  stmxcsr 320(%rsp)
  ldmxcsr 56(%rsp)
  movq %rsp, 0(%r8)
  movq %rbp, 64(%r8)
  leaq CaptureDriverResume(%rip), %rax
  movq %rax, 8(%r8)
  // Expected checked logical selectors; raw host selectors are not inputs.
  movw $8, %ax
  movw %ax, 40(%r8)
  movw $16, %ax
  movw %ax, 42(%r8)
  movw $16, %ax
  movw %ax, 44(%r8)
  movw $16, %ax
  movw %ax, 46(%r8)
  movw $16, %ax
  movw %ax, 48(%r8)
  movw $16, %ax
  movw %ax, 50(%r8)
  // No guest x87 operation runs: the checked initial control is explicit.
  // Separate live-collector tests vary FPCW through the backend state API.
  movw $0x37f, 52(%r8)
  stmxcsr 56(%r8)
  movdqu CaptureVectorSeeds+0(%rip), %xmm0
  movdqu CaptureVectorSeeds+16(%rip), %xmm1
  movdqu CaptureVectorSeeds+32(%rip), %xmm2
  movdqu CaptureVectorSeeds+48(%rip), %xmm3
  movdqu CaptureVectorSeeds+64(%rip), %xmm4
  movdqu CaptureVectorSeeds+80(%rip), %xmm5
  movdqu CaptureVectorSeeds+96(%rip), %xmm6
  movdqu CaptureVectorSeeds+112(%rip), %xmm7
  movdqu CaptureVectorSeeds+128(%rip), %xmm8
  movdqu CaptureVectorSeeds+144(%rip), %xmm9
  movdqu CaptureVectorSeeds+160(%rip), %xmm10
  movdqu CaptureVectorSeeds+176(%rip), %xmm11
  movdqu CaptureVectorSeeds+192(%rip), %xmm12
  movdqu CaptureVectorSeeds+208(%rip), %xmm13
  movdqu CaptureVectorSeeds+224(%rip), %xmm14
  movdqu CaptureVectorSeeds+240(%rip), %xmm15
  movq %rdx, %rcx
  cmpq %rcx, %rcx
  stc
  pushfq
  popq 16(%r8)
  movq $305397760, %rax
  movq $305397762, %rdx
  movq $305397763, %rbx
  movq $305397766, %rsi
  movq $305397767, %rdi
  movq $305397768, %r8
  movq $305397769, %r9
  movq $305397770, %r10
  movq $305397771, %r11
  movq $305397772, %r12
  movq $305397773, %r13
  movq $305397774, %r14
  movq $305397775, %r15
  callq *32(%rsp)
.globl CaptureDriverResume
CaptureDriverResume:
  movq 48(%rsp), %r10
  movq %rax, 24(%r10)
  movq -16(%rsp), %rax
  movq %rax, 80(%r10)
  pushfq
  popq 32(%r10)
  ldmxcsr 320(%rsp)
  movdqu 64(%rsp), %xmm0
  movdqu 80(%rsp), %xmm1
  movdqu 96(%rsp), %xmm2
  movdqu 112(%rsp), %xmm3
  movdqu 128(%rsp), %xmm4
  movdqu 144(%rsp), %xmm5
  movdqu 160(%rsp), %xmm6
  movdqu 176(%rsp), %xmm7
  movdqu 192(%rsp), %xmm8
  movdqu 208(%rsp), %xmm9
  movdqu 224(%rsp), %xmm10
  movdqu 240(%rsp), %xmm11
  movdqu 256(%rsp), %xmm12
  movdqu 272(%rsp), %xmm13
  movdqu 288(%rsp), %xmm14
  movdqu 304(%rsp), %xmm15
  leaq 232(%rbp), %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rdi
  popq %rsi
  popq %rbp
  popq %rbx
  retq
.seh_endproc

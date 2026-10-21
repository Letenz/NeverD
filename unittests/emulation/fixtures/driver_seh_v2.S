//===- driver_seh_v2.S - Independent V2 unwind metadata producer ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
.text
.p2align 4
.globl RaiseFromV2
.seh_proc RaiseFromV2
.seh_unwindversion 2
RaiseFromV2:
  pushq %rbx
  .seh_pushreg %rbx
  pushq %r12
  .seh_pushreg %r12
  subq $40, %rsp
  .seh_stackalloc 40
  .seh_endprologue
  movl $0x12345678, %ebx
  movl $0x76543210, %r12d
  testl %ecx, %ecx
  jnz .Lsecond
  movl $0xc0000005, %ecx
  callq *__imp_ExRaiseStatus(%rip)
  nop
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  popq %r12
  popq %rbx
  .seh_endepilogue
  retq
.Lsecond:
  movl $0x80000002, %ecx
  callq *__imp_ExRaiseStatus(%rip)
  nop
  .seh_startepilogue
  addq $40, %rsp
  .seh_unwindv2start
  popq %r12
  popq %rbx
  .seh_endepilogue
  retq
.seh_endproc

// An independently authored V2 table with nonleading paired markers. These
// records are skipped during prologue unwind and do not define epilogue scopes.
.text
.p2align 4
.globl RaiseFromTrailingV2
RaiseFromTrailingV2:
  pushq %rbx
  pushq %r12
  subq $40, %rsp
.Ltrailing_prologue_end:
  movl $0x12345678, %ebx
  movl $0x76543210, %r12d
  movl $0x80000002, %ecx
  callq *__imp_ExRaiseStatus(%rip)
  nop
  addq $40, %rsp
  popq %r12
  popq %rbx
  retq
.Ltrailing_end:

.section .xdata,"dr"
.p2align 2
.Ltrailing_unwind:
  .byte 2, .Ltrailing_prologue_end-RaiseFromTrailingV2, 5, 0
  .byte 7, 0x42, 3, 0xc0, 1, 0x30
  .byte 0xfe, 0xf6, 0xa5, 0xff
.p2align 2

.section .pdata,"dr"
.p2align 2
  .rva RaiseFromTrailingV2, .Ltrailing_end, .Ltrailing_unwind

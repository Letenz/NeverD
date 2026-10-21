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

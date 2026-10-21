//===- driver_seh_v2.S - Independent V2 unwind metadata producer ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
.text
.p2align 4
.globl RaiseFromV2
RaiseFromV2:
  pushq %rbx
.Lv2_rbx_saved:
  pushq %r12
.Lv2_r12_saved:
  subq $40, %rsp
.Lv2_prologue_end:
  movl $0x12345678, %ebx
  movl $0x76543210, %r12d
  testl %ecx, %ecx
  jnz .Lsecond
  movl $0xc0000005, %ecx
  callq *__imp_ExRaiseStatus(%rip)
  nop
  addq $40, %rsp
.Lv2_first_epilogue:
  popq %r12
  popq %rbx
  retq
.Lsecond:
  movl $0x80000002, %ecx
  callq *__imp_ExRaiseStatus(%rip)
  nop
  addq $40, %rsp
.Lv2_final_epilogue:
  popq %r12
  popq %rbx
  retq
.Lv2_end:

// Encode the same V2 scopes as the newer .seh_unwindversion and epilogue
// directives, keeping this independent fixture usable by older assemblers.
.section .xdata,"dr"
.p2align 2
.Lv2_unwind:
  .byte 2, .Lv2_prologue_end-RaiseFromV2, 5, 0
  .byte .Lv2_end-.Lv2_final_epilogue, 0x16 // UWOP_EPILOG at function end
  .byte .Lv2_end-.Lv2_first_epilogue, 0x06 // UWOP_EPILOG before function end
  .byte .Lv2_prologue_end-RaiseFromV2, 0x42 // UWOP_ALLOC_SMALL: 40 bytes
  .byte .Lv2_r12_saved-RaiseFromV2, 0xc0 // UWOP_PUSH_NONVOL: R12
  .byte .Lv2_rbx_saved-RaiseFromV2, 0x30 // UWOP_PUSH_NONVOL: RBX
.p2align 2

.section .pdata,"dr"
.p2align 2
  .rva RaiseFromV2, .Lv2_end, .Lv2_unwind

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

// Each adjacent five-slot run has an entry and a loop-back consumer.
// Keep the dispatch count and mutation sites independent of loop peeling
// decisions in the host compiler. The O0 C fixture covers compiler output.
#if NEVERD_GOTOFF_THREE_RUNS
#define FUNCTION i386_three_switch_loops
#define RUN_COUNT 3
#else
#define FUNCTION x86tt_twomachine
#define RUN_COUNT 2
#endif

.text
.globl FUNCTION
.type FUNCTION, @function
FUNCTION:
  pushl %ebx
  call .Lpc
.Lpc:
  popl %ebx
  .byte 0x81, 0xc3
.Lgot:
  .long .Lgot - .Lpc
  .reloc .Lgot, R_386_GOTPC, _GLOBAL_OFFSET_TABLE_
  xorl %eax, %eax

.macro run id, next
.Lrun\id:
  movl $0, %edx
  .if \id == 0
  // This dead scalar immediate is never a table-load operand.
  .globl x86tt_extra_reference
x86tt_extra_reference:
  movl $17, %ecx
  .endif
  movl .Ltable\id@GOTOFF(%ebx,%edx,4), %ecx
  addl %ebx, %ecx
  jmp *%ecx
.Lback\id:
  movl .Ltable\id@GOTOFF(%ebx,%edx,4), %ecx
  addl %ebx, %ecx
  .if \id == RUN_COUNT - 1
  .globl x86tt_final_dispatch
x86tt_final_dispatch:
  .endif
  jmp *%ecx
.Lcase\id\()_0:
  addl $(\id * 10 + 1), %eax
  movl $1, %edx
  jmp .Lback\id
.Lcase\id\()_1:
  addl $(\id * 10 + 2), %eax
  movl $2, %edx
  jmp .Lback\id
.Lcase\id\()_2:
  addl $(\id * 10 + 3), %eax
  movl $3, %edx
  jmp .Lback\id
.Lcase\id\()_3:
  addl $(\id * 10 + 4), %eax
  movl $4, %edx
  jmp .Lback\id
.Lcase\id\()_4:
  addl $(\id * 10 + 5), %eax
  jmp \next
.endm

  run 0, .Lrun1
#if NEVERD_GOTOFF_THREE_RUNS
  run 1, .Lrun2
  run 2, .Ldone
#else
  run 1, .Ldone
#endif
.Ldone:
  popl %ebx
  ret
.size FUNCTION, .-FUNCTION

// Local labels intentionally leave adjacent runs under one section owner.
.section .rodata
.p2align 2
.macro table id
.Ltable\id:
  .long .Lcase\id\()_0@GOTOFF
  .long .Lcase\id\()_1@GOTOFF
  .long .Lcase\id\()_2@GOTOFF
  .long .Lcase\id\()_3@GOTOFF
  .long .Lcase\id\()_4@GOTOFF
.endm
  table 0
  table 1
#if NEVERD_GOTOFF_THREE_RUNS
  table 2
#endif

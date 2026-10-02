// Independently authored repeated transfers over overlapping guest stack
// ranges. LEA/MOV and string moves preserve arithmetic flags.
.text
.macro string_case name, instruction, fill, backward
.globl \name
.type \name,@function
\name:
  .if \backward
    .if !\fill
      leaq -64(%rsp), %rsi
    .endif
    leaq -65(%rsp), %rdi
    std
  .else
    .if !\fill
      leaq -96(%rsp), %rsi
    .endif
    leaq -95(%rsp), %rdi
    cld
  .endif
  movl $3, %ecx
  rep \instruction
  retq
.size \name, .-\name
.endm

string_case generic_string_move_b_f, movsb, 0, 0
string_case generic_string_move_b_b, movsb, 0, 1
string_case generic_string_fill_b_f, stosb, 1, 0
string_case generic_string_fill_b_b, stosb, 1, 1
string_case generic_string_move_w_f, movsw, 0, 0
string_case generic_string_move_w_b, movsw, 0, 1
string_case generic_string_fill_w_f, stosw, 1, 0
string_case generic_string_fill_w_b, stosw, 1, 1
string_case generic_string_move_l_f, movsl, 0, 0
string_case generic_string_move_l_b, movsl, 0, 1
string_case generic_string_fill_l_f, stosl, 1, 0
string_case generic_string_fill_l_b, stosl, 1, 1
string_case generic_string_move_q_f, movsq, 0, 0
string_case generic_string_move_q_b, movsq, 0, 1
string_case generic_string_fill_q_f, stosq, 1, 0
string_case generic_string_fill_q_b, stosq, 1, 1

.globl generic_string_zero
.type generic_string_zero,@function
generic_string_zero:
  movl $0, %ecx
  rep movsb
  retq
.size generic_string_zero, .-generic_string_zero
.section .note.GNU-stack,"",@progbits

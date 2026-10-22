// Long upstream arithmetic must not obscure the exact register lane checked
// by the guard. The fourth physical slot is outside the admitted domain.
.text
.macro NARROW_GUARD name, mode=0
.globl \name
.type \name,@function
\name:
  movl %edi, %edx
  .rept 100
  addl $1, %edx
  .endr
  leal -76(%edx), %eax
  .if \mode == 1
  cmpb $2, %ah
  .elseif \mode == 2
  cmpw $2, %ax
  .else
  cmpb $2, %al
  .endif
  ja .Ldefault\@
  .if \mode == 3
  movb %cl, %al
  .elseif \mode == 4
  movl %ecx, %eax
  .elseif \mode == 7
  movb %cl, %ah
  .elseif \mode == 8
  call narrow_guard_barrier
  .endif
  .if \mode == 1 || \mode == 5
  movzbl %ah, %eax
  .elseif \mode == 2
  movzwl %ax, %eax
  .elseif \mode != 6
  movzbl %al, %eax
  .endif
  leaq .Ltable\@(%rip), %rdx
  movslq (%rdx,%rax,4), %rax
  addq %rdx,%rax
  jmp *%rax
.Lcase0\@: movl $710, %eax
  ret
.Lcase1\@: movl $711, %eax
  ret
.Lcase2\@: movl $712, %eax
  ret
.Lpoison\@: movl $799, %eax
  ret
.Ldefault\@:
  .if \mode == 9
  .rept 200
  addl $1, %edx
  .endr
  movl %edx, %eax
  .else
  movl $798, %eax
  .endif
  ret
.size \name,.-\name
.pushsection .rodata,"a",@progbits
.p2align 2
.Ltable\@:
  .long .Lcase0\@-.Ltable\@,.Lcase1\@-.Ltable\@,.Lcase2\@-.Ltable\@,.Lpoison\@-.Ltable\@
.popsection
.endm
NARROW_GUARD narrow_guard_low
NARROW_GUARD narrow_guard_high, 1
NARROW_GUARD narrow_guard_word, 2
NARROW_GUARD narrow_guard_changed_low, 3
NARROW_GUARD narrow_guard_changed_full, 4
NARROW_GUARD narrow_guard_other_lane, 5
NARROW_GUARD narrow_guard_unextended, 6
NARROW_GUARD narrow_guard_disjoint_write, 7
NARROW_GUARD narrow_guard_call, 8
NARROW_GUARD narrow_guard_scalar_return, 9
.globl narrow_guard_barrier
.type narrow_guard_barrier,@function
narrow_guard_barrier:
  ret
.size narrow_guard_barrier,.-narrow_guard_barrier

// A stored literal proposes a guard bound only after its exact reload proves it.
.macro RELOADED_GUARD name, mode=0
.globl \name
.type \name,@function
\name:
  pushq %rbp
  movq %rsp,%rbp
  movq %rdi,%rax
  .if \mode == 7
  movq %rax,%rdx
  andq $24,%rdx
  subq %rdx,%rsp
  .endif
  .if \mode == 4
  subq $8,%rsp
  pushq %rcx
  movq $2,8(%rsp)
  .elseif \mode == 1
  pushq $3
  .elseif \mode == 12
  pushq $narrow_guard_low+2
  .else
  pushq $2
  .endif
  .if \mode == 2
  movq %rdx,(%rsp)
  .elseif \mode == 3
  call narrow_guard_barrier
  .elseif \mode == 5
  movb $1,(%rsp)
  .elseif \mode == 9
  addq $8,%rsp
  .elseif \mode == 10
  movw $0,%sp
  .elseif \mode == 11
  lock addq $1,(%rsp)
  .endif
.Lpop\@:
  .if \mode == 8
  popq %rdx
  movq %rdx,%rcx
  .else
  popq %rcx
  .endif
  .if \mode == 4
  addq $8,%rsp
  .endif
  .if \mode == 13
  movq $narrow_guard_low+2,%rcx
  .endif
  cmpq %rcx,%rax
  ja .Lreload_default\@
  jmp *.Lreload_table\@(,%rax,8)
.Lreload_case0\@: movl $710,%eax
  jmp .Lreload_return\@
.Lreload_case1\@: movl $711,%eax
  jmp .Lreload_return\@
.Lreload_case2\@: movl $712,%eax
  jmp .Lreload_return\@
.Lreload_case3\@: movl $799,%eax
  jmp .Lreload_return\@
.Lreload_default\@: movl $798,%eax
.Lreload_return\@:
  movq %rbp,%rsp
  popq %rbp
  ret
.size \name,.-\name
.pushsection .rodata,"a",@progbits
.p2align 3
.Lreload_table\@:
  .quad .Lreload_case0\@,.Lreload_case1\@,.Lreload_case2\@,.Lreload_case3\@
  .if \mode == 6
  .quad .Lpop\@
  .endif
.popsection
.endm
RELOADED_GUARD reloaded_guard
RELOADED_GUARD reloaded_guard_changed, 1
RELOADED_GUARD reloaded_guard_clobbered, 2
RELOADED_GUARD reloaded_guard_call, 3
RELOADED_GUARD reloaded_guard_wrong_slot, 4
RELOADED_GUARD reloaded_guard_partial, 5
RELOADED_GUARD reloaded_guard_root, 6
RELOADED_GUARD reloaded_guard_pivot, 7
RELOADED_GUARD reloaded_guard_copy, 8
RELOADED_GUARD reloaded_guard_changed_sp, 9
RELOADED_GUARD reloaded_guard_partial_sp, 10
RELOADED_GUARD reloaded_guard_atomic, 11
RELOADED_GUARD reloaded_guard_address, 12
RELOADED_GUARD reloaded_guard_direct_address, 13

// Long upstream arithmetic must not obscure the exact register lane checked
// by the guard. The fourth physical slot is outside the admitted domain.
.text
.macro NARROW_GUARD name, mode=0
.globl \name
.type \name,@function
\name:
  movl 4(%esp), %edx
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
  jmp *.Ltable\@(,%eax,4)
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
  .long .Lcase0\@,.Lcase1\@,.Lcase2\@,.Lpoison\@
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
  pushl %ebp
  movl %esp,%ebp
  movl 8(%ebp),%eax
  .if \mode == 7
  movl %eax,%edx
  andl $24,%edx
  subl %edx,%esp
  .endif
  .if \mode == 4
  subl $4,%esp
  pushl %ecx
  movl $2,4(%esp)
  .elseif \mode == 1
  pushl $3
  .elseif \mode == 12
  pushl $narrow_guard_low+2
  .else
  pushl $2
  .endif
  .if \mode == 2
  movl %edx,(%esp)
  .elseif \mode == 3
  call narrow_guard_barrier
  .elseif \mode == 5
  movb $1,(%esp)
  .elseif \mode == 9
  addl $4,%esp
  .elseif \mode == 10
  movw $0,%sp
  .elseif \mode == 11
  lock addl $1,(%esp)
  .endif
.Lpop\@:
  .if \mode == 8
  popl %edx
  movl %edx,%ecx
  .else
  popl %ecx
  .endif
  .if \mode == 4
  addl $4,%esp
  .endif
  .if \mode == 13
  movl $narrow_guard_low+2,%ecx
  .endif
  cmpl %ecx,%eax
  ja .Lreload_default\@
  jmp *.Lreload_table\@(,%eax,4)
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
  movl %ebp,%esp
  popl %ebp
  ret
.size \name,.-\name
.pushsection .rodata,"a",@progbits
.p2align 2
.Lreload_table\@:
  .long .Lreload_case0\@,.Lreload_case1\@,.Lreload_case2\@,.Lreload_case3\@
  .if \mode == 6
  .long .Lpop\@
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

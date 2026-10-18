# The GOT base is spilled at -8(%ebp) before a dynamic allocation moves ESP
# by a register amount.  The loop then stores through an address formed from
# that ESP with an index nothing bounds, which may reach the spill, before it
# reloads the GOT base for a GOTOFF switch table.
	.text
	.globl	jt_i386_dynamic_stack_store
	.type	jt_i386_dynamic_stack_store,@function
jt_i386_dynamic_stack_store:
	pushl	%ebp
	movl	%esp, %ebp
	subl	$8, %esp
	calll	.Ldyn_pc
.Ldyn_pc:
	popl	%eax
.Ldyn_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Ldyn_got-.Ldyn_pc), %eax
	movl	%eax, -8(%ebp)
	movl	8(%ebp), %ecx
	subl	%ecx, %esp
.Ldyn_loop:
	movl	12(%ebp), %edx
	movl	(%edx), %ecx
	movl	4(%edx), %eax
	addl	$8, %edx
	movl	%edx, 12(%ebp)
	movl	%esp, %edx
	movl	$5, (%edx,%eax,4)
	cmpl	$3, %ecx
	ja	.Ldyn_default
	movl	-8(%ebp), %eax
	movl	.Ldyn_table@GOTOFF(%eax,%ecx,4), %ecx
	addl	%eax, %ecx
	jmpl	*%ecx
.Ldyn_case0:
	movl	$10, %eax
	jmp	.Ldyn_done
.Ldyn_case1:
	jmp	.Ldyn_loop
.Ldyn_case2:
	jmp	.Ldyn_loop
.Ldyn_case3:
	jmp	.Ldyn_loop
.Ldyn_default:
	movl	$-1, %eax
.Ldyn_done:
	movl	%ebp, %esp
	popl	%ebp
	retl
	.size	jt_i386_dynamic_stack_store, .-jt_i386_dynamic_stack_store

	.section	.rodata,"a",@progbits
	.p2align	2
.Ldyn_table:
	.long	.Ldyn_case0@GOTOFF
	.long	.Ldyn_case1@GOTOFF
	.long	.Ldyn_case2@GOTOFF
	.long	.Ldyn_case3@GOTOFF

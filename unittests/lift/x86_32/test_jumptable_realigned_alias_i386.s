# Both functions align ESP, spill the GOT base to 8(%esp) of the aligned
# frame and reload it for a GOTOFF switch table.  Before the reload, one of
# them stores to 8(%ebp), the incoming argument, which lies above every cell
# the aligned frame may hold.  The other stores to -12(%ebp), which may be
# the very cell 8(%esp) names, depending on the alignment the caller left.
	.text
	.globl	jt_i386_realigned_arg_store
	.type	jt_i386_realigned_arg_store,@function
jt_i386_realigned_arg_store:
	pushl	%ebp
	movl	%esp, %ebp
	andl	$-16, %esp
	subl	$16, %esp
	calll	.Larg_pc
.Larg_pc:
	popl	%eax
.Larg_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Larg_got-.Larg_pc), %eax
	movl	%eax, 8(%esp)
	movl	8(%ebp), %ecx
	movl	$0, 8(%ebp)
	cmpl	$3, %ecx
	ja	.Larg_default
	movl	8(%esp), %eax
	movl	.Larg_table@GOTOFF(%eax,%ecx,4), %ecx
	addl	%eax, %ecx
	jmpl	*%ecx
.Larg_case0:
	movl	$10, %eax
	jmp	.Larg_done
.Larg_case1:
	movl	$11, %eax
	jmp	.Larg_done
.Larg_case2:
	movl	$12, %eax
	jmp	.Larg_done
.Larg_case3:
	movl	$13, %eax
	jmp	.Larg_done
.Larg_default:
	movl	$-1, %eax
.Larg_done:
	movl	%ebp, %esp
	popl	%ebp
	retl
	.size	jt_i386_realigned_arg_store, .-jt_i386_realigned_arg_store

	.globl	jt_i386_realigned_alias_store
	.type	jt_i386_realigned_alias_store,@function
jt_i386_realigned_alias_store:
	pushl	%ebp
	movl	%esp, %ebp
	andl	$-16, %esp
	subl	$16, %esp
	calll	.Lalias_pc
.Lalias_pc:
	popl	%eax
.Lalias_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lalias_got-.Lalias_pc), %eax
	movl	%eax, 8(%esp)
	movl	8(%ebp), %ecx
	movl	$0, -12(%ebp)
	cmpl	$3, %ecx
	ja	.Lalias_default
	movl	8(%esp), %eax
	movl	.Lalias_table@GOTOFF(%eax,%ecx,4), %ecx
	addl	%eax, %ecx
	jmpl	*%ecx
.Lalias_case0:
	movl	$10, %eax
	jmp	.Lalias_done
.Lalias_case1:
	movl	$11, %eax
	jmp	.Lalias_done
.Lalias_case2:
	movl	$12, %eax
	jmp	.Lalias_done
.Lalias_case3:
	movl	$13, %eax
	jmp	.Lalias_done
.Lalias_default:
	movl	$-1, %eax
.Lalias_done:
	movl	%ebp, %esp
	popl	%ebp
	retl
	.size	jt_i386_realigned_alias_store, .-jt_i386_realigned_alias_store

	.section	.rodata,"a",@progbits
	.p2align	2
.Larg_table:
	.long	.Larg_case0@GOTOFF
	.long	.Larg_case1@GOTOFF
	.long	.Larg_case2@GOTOFF
	.long	.Larg_case3@GOTOFF
.Lalias_table:
	.long	.Lalias_case0@GOTOFF
	.long	.Lalias_case1@GOTOFF
	.long	.Lalias_case2@GOTOFF
	.long	.Lalias_case3@GOTOFF

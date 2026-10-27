// A function with a masked relative table and a register jump.
//
// `tail_dispatch` jumps through a pointer it loads only after releasing its
// frame, as rustc's `<dyn Trait>::method` tail calls do: the jump leaves the
// function, so the table stays.  The call before the release keeps RSP.
//
// `framed_dispatch` jumps with its frame still allocated: that jump may
// enter any of the function's code, the dispatch past its mask included, so
// the table is withdrawn.
	.text
	.globl	tail_dispatch
	.type	tail_dispatch,@function
tail_dispatch:
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%r15
	pushq	%r14
	pushq	%rbx
	subq	$0xc8, %rsp
	movq	(%rdi), %rdi
	testb	$4, %dil
	jne	.Ltail
	movl	%edi, %eax
	andl	$3, %eax
	leaq	.Ltable(%rip), %rcx
	movslq	(%rcx,%rax,4), %rax
	addq	%rcx, %rax
	jmpq	*%rax
.Lc0:	movl	$10, %eax
	jmp	.Lout
.Lc1:	movl	$11, %eax
	jmp	.Lout
.Lc2:	movl	$12, %eax
	jmp	.Lout
.Lc3:	movl	$13, %eax
.Lout:
	addq	$0xc8, %rsp
	popq	%rbx
	popq	%r14
	popq	%r15
	popq	%rbp
	retq
.Ltail:
	movq	%rdi, %rbx
	callq	*0x18(%rdi)
	movq	7(%rbx), %rcx
	movq	0x20(%rcx), %rcx
	movq	%rbx, %rdi
	addq	$0xc8, %rsp
	popq	%rbx
	popq	%r14
	popq	%r15
	popq	%rbp
	jmpq	*%rcx
	.size	tail_dispatch, .-tail_dispatch

	.globl	framed_dispatch
	.type	framed_dispatch,@function
framed_dispatch:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$0x10, %rsp
	movq	(%rdi), %rdi
	testb	$4, %dil
	jne	.Lframed_jump
	movl	%edi, %eax
	andl	$3, %eax
	leaq	.Lframed_table(%rip), %rcx
	movslq	(%rcx,%rax,4), %rax
	addq	%rcx, %rax
	jmpq	*%rax
.Lf0:	movl	$20, %eax
	jmp	.Lframed_out
.Lf1:	movl	$21, %eax
	jmp	.Lframed_out
.Lf2:	movl	$22, %eax
	jmp	.Lframed_out
.Lf3:	movl	$23, %eax
.Lframed_out:
	addq	$0x10, %rsp
	popq	%rbp
	retq
.Lframed_jump:
	movq	0x20(%rdi), %rcx
	jmpq	*%rcx
	.size	framed_dispatch, .-framed_dispatch

	.section	.rodata
	.p2align	2
.Ltable:
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
.Lframed_table:
	.long	.Lf0-.Lframed_table
	.long	.Lf1-.Lframed_table
	.long	.Lf2-.Lframed_table
	.long	.Lf3-.Lframed_table
	.section	.note.GNU-stack,"",@progbits

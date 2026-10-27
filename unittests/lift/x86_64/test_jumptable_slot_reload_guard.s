// A switch whose byte lives in a frame slot, as rustc keeps an enum tag.
//
// `slot_dispatch` compares the slot itself and then reloads the byte from
// it for the table: the two loads read the slot with no store and no call
// between them, so they are one value and the comparison bounds the table.
// The call before them leaves what the slot holds unknown.
//
// `slot_dispatch_after_call` makes a call between the two loads: the callee
// may write the slot, so the comparison bounds nothing.
//
// Each table follows its function in .text, as Mach-O keeps them, so no
// section end bounds it.
	.text
	.globl	slot_dispatch
	.type	slot_dispatch,@function
slot_dispatch:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$0x40, %rsp
	movl	%edi, -0x38(%rbp)
	movq	%rsi, %rdi
	callq	*%rdx
	cmpb	$3, -0x38(%rbp)
	ja	.Ldefault
	movq	-0x10(%rbp), %rdx
	movzbl	-0x38(%rbp), %edx
	leaq	.Ltable(%rip), %rsi
	movslq	(%rsi,%rdx,4), %rdx
	addq	%rsi, %rdx
	jmpq	*%rdx
.Lc0:	movl	$10, %eax
	jmp	.Lout
.Lc1:	movl	$11, %eax
	jmp	.Lout
.Lc2:	movl	$12, %eax
	jmp	.Lout
.Lc3:	movl	$13, %eax
	jmp	.Lout
.Ldefault:
	movl	$-1, %eax
.Lout:
	addq	$0x40, %rsp
	popq	%rbp
	retq
	.p2align	2
.Ltable:
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.size	slot_dispatch, .-slot_dispatch

	.globl	slot_dispatch_after_call
	.type	slot_dispatch_after_call,@function
slot_dispatch_after_call:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$0x40, %rsp
	movl	%edi, -0x38(%rbp)
	cmpb	$3, -0x38(%rbp)
	ja	.Ldefault2
	leaq	-0x38(%rbp), %rdi
	callq	*%rsi
	movzbl	-0x38(%rbp), %edx
	leaq	.Ltable2(%rip), %rsi
	movslq	(%rsi,%rdx,4), %rdx
	addq	%rsi, %rdx
	jmpq	*%rdx
.Ld0:	movl	$20, %eax
	jmp	.Lout2
.Ld1:	movl	$21, %eax
	jmp	.Lout2
.Ld2:	movl	$22, %eax
	jmp	.Lout2
.Ld3:	movl	$23, %eax
	jmp	.Lout2
.Ldefault2:
	movl	$-1, %eax
.Lout2:
	addq	$0x40, %rsp
	popq	%rbp
	retq
	.p2align	2
.Ltable2:
	.long	.Ld0-.Ltable2
	.long	.Ld1-.Ltable2
	.long	.Ld2-.Ltable2
	.long	.Ld3-.Ltable2
	.size	slot_dispatch_after_call, .-slot_dispatch_after_call


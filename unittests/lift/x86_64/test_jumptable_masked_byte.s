// The bound comes from a compare of the masked low byte; the index is the
// mask of the zero-extended byte, computed again for the table read, as in
// libc++abi's readEncodedPointer on x86-64.
	.text
	.globl	masked_byte_dispatch
	.type	masked_byte_dispatch,@function
masked_byte_dispatch:
	movl	%esi, %eax
	andb	$0xf, %al
	cmpb	$0xc, %al
	ja	.Ldefault
	movzbl	%sil, %eax
	andl	$0xf, %eax
	leaq	.Ltable(%rip), %rcx
	movslq	(%rcx,%rax,4), %rax
	addq	%rcx, %rax
	jmpq	*%rax
.Lc0:
	movl	$10, %eax
	retq
.Lc1:
	movl	$11, %eax
	retq
.Lc2:
	movl	$12, %eax
	retq
.Lc3:
	movl	$13, %eax
	retq
.Lc4:
	movl	$14, %eax
	retq
.Lc5:
	movl	$15, %eax
	retq
.Lc6:
	movl	$16, %eax
	retq
.Lc7:
	movl	$17, %eax
	retq
.Lc8:
	movl	$18, %eax
	retq
.Lc9:
	movl	$19, %eax
	retq
.Lc10:
	movl	$20, %eax
	retq
.Lc11:
	movl	$21, %eax
	retq
.Lc12:
	movl	$22, %eax
	retq
.Ldefault:
	xorl	%eax, %eax
	retq
	.size	masked_byte_dispatch, .-masked_byte_dispatch

	.section	.rodata
	.p2align	2
.Ltable:
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc8-.Ltable
	.long	.Lc9-.Ltable
	.long	.Lc10-.Ltable
	.long	.Lc11-.Ltable
	.long	.Lc12-.Ltable
// The words after the table are no entries: read as entries, they would
// name addresses far outside the code.
	.long	0x40000000
	.long	0x40000000
	.long	0x40000000
	.section	.note.GNU-stack,"",@progbits

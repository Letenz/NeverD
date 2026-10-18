// clang -O0 code for a switch on an unsigned char that covers every value:
// the byte is zero-extended and spilled, and the 256-entry table is read with
// the reloaded value. No compare and no mask bounds the index.
	.text
	.globl	byte_switch
	.type	byte_switch,@function
byte_switch:
	pushq	%rbp
	movq	%rsp, %rbp
	movb	%dil, -0x1(%rbp)
	movzbl	-0x1(%rbp), %eax
	movl	%eax, %ecx
	movq	%rcx, -0x10(%rbp)
	leaq	.Ltable(%rip), %rax
	movq	-0x10(%rbp), %rcx
	movslq	(%rax,%rcx,4), %rdx
	addq	%rax, %rdx
	jmpq	*%rdx
.Lc0:
	movl	$10, %eax
	popq	%rbp
	retq
.Lc1:
	movl	$11, %eax
	popq	%rbp
	retq
.Lc2:
	movl	$12, %eax
	popq	%rbp
	retq
.Lc3:
	movl	$13, %eax
	popq	%rbp
	retq
.Lc4:
	movl	$14, %eax
	popq	%rbp
	retq
.Lc5:
	movl	$15, %eax
	popq	%rbp
	retq
.Lc6:
	movl	$16, %eax
	popq	%rbp
	retq
.Lc7:
	movl	$17, %eax
	popq	%rbp
	retq
	.size	byte_switch, .-byte_switch

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
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
	.long	.Lc0-.Ltable
	.long	.Lc1-.Ltable
	.long	.Lc2-.Ltable
	.long	.Lc3-.Ltable
	.long	.Lc4-.Ltable
	.long	.Lc5-.Ltable
	.long	.Lc6-.Ltable
	.long	.Lc7-.Ltable
// Other data follows the table.
.Lmessage:
	.asciz	"byte switch"
	.section	.note.GNU-stack,"",@progbits

# plt_iszero keeps strcmp's zero test in DL, as GCC does for a flag it uses
# later: `sete dl` writes the low byte of RDX, which the lift reads to keep
# the bytes it does not write.  strcmp, called through its PLT entry,
# returns one int.
#   int plt_iszero(const char *a, const char *b) { return strcmp(a, b) == 0; }

	.text
	.p2align 4
	.globl	plt_iszero
	.type	plt_iszero, @function
plt_iszero:
	pushq	%rbx
	call	strcmp@PLT
	testl	%eax, %eax
	sete	%dl
	movzbl	%dl, %eax
	popq	%rbx
	ret
	.size	plt_iszero, .-plt_iszero
	.section	.note.GNU-stack,"",@progbits

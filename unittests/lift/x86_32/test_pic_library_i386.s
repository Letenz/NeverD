// An i386 shared library as GCC builds one (gcc -m32 -O2 -fPIC):
//   int put(const char *s) { return fputs(s, stdout); }
// Its PLT entry jumps through the GOT by the address the caller holds in
// %ebx, and the function finds that address through a get-PC thunk.

	.text
	.p2align 4
	.globl	put
	.type	put, @function
put:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	subl	$16, %esp
	movl	stdout@GOT(%ebx), %eax
	pushl	(%eax)
	pushl	28(%esp)
	call	fputs@PLT
	addl	$24, %esp
	popl	%ebx
	ret
	.size	put, .-put

	.section	.text.__x86.get_pc_thunk.bx,"axG",@progbits,__x86.get_pc_thunk.bx,comdat
	.globl	__x86.get_pc_thunk.bx
	.hidden	__x86.get_pc_thunk.bx
	.type	__x86.get_pc_thunk.bx, @function
__x86.get_pc_thunk.bx:
	movl	(%esp), %ebx
	ret

	.section	.note.GNU-stack,"",@progbits

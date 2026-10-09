// An i386 shared library as GCC builds one (gcc -m32 -O2 -fPIC):
//   int put(const char *s) { return fputs(s, stdout); }
//   static int factor = 3;
//   int scale(int a, int b) { return (a + b) * factor; }
//   int twice(int a) { return a + a + factor; }
// Its PLT entry jumps through the GOT by the address the caller holds in
// %ebx.  Each function finds that address with a call to a get-PC thunk,
// which loads one register with its return address and leaves the others as
// they were, so `scale` and `twice` keep a value in %ecx across the call.
// `twice` calls a thunk padded as GCC pads one for in-order Atom
// (-mtune=bonnell).

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

	.p2align 4
	.globl	scale
	.type	scale, @function
scale:
	movl	4(%esp), %ecx
	addl	8(%esp), %ecx
	call	__x86.get_pc_thunk.ax
	addl	$_GLOBAL_OFFSET_TABLE_, %eax
	imull	factor@GOTOFF(%eax), %ecx
	movl	%ecx, %eax
	ret
	.size	scale, .-scale

	.p2align 4
	.globl	twice
	.type	twice, @function
twice:
	movl	4(%esp), %ecx
	addl	%ecx, %ecx
	call	__x86.get_pc_thunk.dx
	addl	$_GLOBAL_OFFSET_TABLE_, %edx
	movl	factor@GOTOFF(%edx), %eax
	addl	%ecx, %eax
	ret
	.size	twice, .-twice

	.data
	.p2align 2
	.type	factor, @object
	.size	factor, 4
factor:
	.long	3

	.section	.text.__x86.get_pc_thunk.bx,"axG",@progbits,__x86.get_pc_thunk.bx,comdat
	.globl	__x86.get_pc_thunk.bx
	.hidden	__x86.get_pc_thunk.bx
	.type	__x86.get_pc_thunk.bx, @function
__x86.get_pc_thunk.bx:
	movl	(%esp), %ebx
	ret

	.section	.text.__x86.get_pc_thunk.ax,"axG",@progbits,__x86.get_pc_thunk.ax,comdat
	.globl	__x86.get_pc_thunk.ax
	.hidden	__x86.get_pc_thunk.ax
	.type	__x86.get_pc_thunk.ax, @function
__x86.get_pc_thunk.ax:
	movl	(%esp), %eax
	ret

	.section	.text.__x86.get_pc_thunk.dx,"axG",@progbits,__x86.get_pc_thunk.dx,comdat
	.globl	__x86.get_pc_thunk.dx
	.hidden	__x86.get_pc_thunk.dx
	.type	__x86.get_pc_thunk.dx, @function
__x86.get_pc_thunk.dx:
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	movl	(%esp), %edx
	ret

	.section	.note.GNU-stack,"",@progbits

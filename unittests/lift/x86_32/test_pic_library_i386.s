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
// (-mtune=bonnell).  Each function has the unwind record GCC gives it.
// Clang finds the GOT by calling the next instruction and popping the return
// address, here in -O0 frames realigned to 16 bytes (clang -m32 -O0 -fPIC
// -mstackrealign):
//   int clang_scale(int a) { return a * factor; }
//   int clang_lower(char c) { return tolower((unsigned char)c); }
// `clang_scale` reads `factor` through the popped address; `clang_lower`
// keeps it in %ebx only for its PLT call.  A label marks each pop.
//   int clang_reload(int c) { return tolower(c) + factor; }
// spills the GOT address before its PLT call and reloads it to read `factor`.

	.text
	.p2align 4
	.globl	put
	.type	put, @function
put:
	.cfi_startproc
	pushl	%ebx
	.cfi_def_cfa_offset 8
	.cfi_offset 3, -8
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	subl	$16, %esp
	.cfi_def_cfa_offset 24
	movl	stdout@GOT(%ebx), %eax
	pushl	(%eax)
	.cfi_def_cfa_offset 28
	pushl	28(%esp)
	.cfi_def_cfa_offset 32
	call	fputs@PLT
	addl	$24, %esp
	.cfi_def_cfa_offset 8
	popl	%ebx
	.cfi_restore 3
	.cfi_def_cfa_offset 4
	ret
	.cfi_endproc
	.size	put, .-put

	.p2align 4
	.globl	scale
	.type	scale, @function
scale:
	.cfi_startproc
	movl	4(%esp), %ecx
	addl	8(%esp), %ecx
	call	__x86.get_pc_thunk.ax
	addl	$_GLOBAL_OFFSET_TABLE_, %eax
	imull	factor@GOTOFF(%eax), %ecx
	movl	%ecx, %eax
	ret
	.cfi_endproc
	.size	scale, .-scale

	.p2align 4
	.globl	twice
	.type	twice, @function
twice:
	.cfi_startproc
	movl	4(%esp), %ecx
	addl	%ecx, %ecx
	call	__x86.get_pc_thunk.dx
	addl	$_GLOBAL_OFFSET_TABLE_, %edx
	movl	factor@GOTOFF(%edx), %eax
	addl	%ecx, %eax
	ret
	.cfi_endproc
	.size	twice, .-twice

	.p2align 4
	.globl	clang_scale
	.type	clang_scale, @function
clang_scale:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	andl	$-16, %esp
	subl	$16, %esp
	calll	clang_scale_pop
clang_scale_pop:
	popl	%eax
.Lscale_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lscale_got-clang_scale_pop), %eax
	movl	8(%ebp), %ecx
	movl	%ecx, 12(%esp)
	movl	12(%esp), %ecx
	imull	factor@GOTOFF(%eax), %ecx
	movl	%ecx, %eax
	movl	%ebp, %esp
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_scale, .-clang_scale

	.p2align 4
	.globl	clang_lower
	.type	clang_lower, @function
clang_lower:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	pushl	%ebx
	andl	$-16, %esp
	subl	$32, %esp
	.cfi_offset %ebx, -12
	calll	clang_lower_pop
clang_lower_pop:
	popl	%eax
.Llower_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Llower_got-clang_lower_pop), %eax
	movb	8(%ebp), %cl
	movzbl	8(%ebp), %edx
	movl	%edx, (%esp)
	movl	%eax, %ebx
	movb	%cl, 19(%esp)
	calll	tolower@PLT
	leal	-4(%ebp), %esp
	popl	%ebx
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_lower, .-clang_lower

	.p2align 4
	.globl	clang_reload
	.type	clang_reload, @function
clang_reload:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	pushl	%ebx
	andl	$-16, %esp
	subl	$16, %esp
	.cfi_offset %ebx, -12
	calll	clang_reload_pop
clang_reload_pop:
	popl	%eax
.Lreload_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lreload_got-clang_reload_pop), %eax
	movl	%eax, 8(%esp)
	movl	8(%ebp), %ecx
	movl	%ecx, (%esp)
	movl	%eax, %ebx
	calll	tolower@PLT
	movl	8(%esp), %ecx
	addl	factor@GOTOFF(%ecx), %eax
	leal	-4(%ebp), %esp
	popl	%ebx
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_reload, .-clang_reload

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
	.cfi_startproc
	movl	(%esp), %ebx
	ret
	.cfi_endproc

	.section	.text.__x86.get_pc_thunk.ax,"axG",@progbits,__x86.get_pc_thunk.ax,comdat
	.globl	__x86.get_pc_thunk.ax
	.hidden	__x86.get_pc_thunk.ax
	.type	__x86.get_pc_thunk.ax, @function
__x86.get_pc_thunk.ax:
	.cfi_startproc
	movl	(%esp), %eax
	ret
	.cfi_endproc

	.section	.text.__x86.get_pc_thunk.dx,"axG",@progbits,__x86.get_pc_thunk.dx,comdat
	.globl	__x86.get_pc_thunk.dx
	.hidden	__x86.get_pc_thunk.dx
	.type	__x86.get_pc_thunk.dx, @function
__x86.get_pc_thunk.dx:
	.cfi_startproc
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
	.cfi_endproc

	.section	.note.GNU-stack,"",@progbits

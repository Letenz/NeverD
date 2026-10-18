// Switch tables of an i386 shared library.  PIC code finds the GOT address
// with a get-PC sequence and an `add $_GLOBAL_OFFSET_TABLE_`, reads a table
// of `case@GOTOFF` entries relative to it, and adds the entry back to it:
//   gcc_switch        gcc -m32 -O2 -fPIC: thunk, `mov tbl@GOTOFF(...)`, add
//   gcc_fused_switch  gcc's fused `add tbl@GOTOFF(%eax,%edx,4), %eax`
//   clang_switch      clang -m32 -O2 -fPIC: call/pop, fused add
//   saved_got_switch  the GOT register set before a branch around a call
// The linked library keeps no relocation for any of them; the PLT call gives
// it a DT_PLTGOT besides its `_GLOBAL_OFFSET_TABLE_` symbol.  Two dispatches
// whose register is not the GOT address on every path must stay unresolved:
//   offset_anchor_switch     adds GOT+4, so every target is a case + 4
//   clobbered_anchor_switch  one path replaces the GOT register

	.text
	.p2align 4
	.globl	gcc_switch
	.type	gcc_switch, @function
gcc_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	movl	8(%esp), %eax
	cmpl	$4, %eax
	ja	.Lgs_default
	movl	.Lgs_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
gcc_switch_dispatch:
	jmp	*%eax
gcc_switch_case0:
	movl	$10, %eax
	popl	%ebx
	ret
gcc_switch_case1:
	movl	$21, %eax
	popl	%ebx
	ret
gcc_switch_case2:
	movl	$32, %eax
	popl	%ebx
	ret
gcc_switch_case3:
	movl	$43, %eax
	popl	%ebx
	ret
gcc_switch_case4:
	movl	$54, %eax
	popl	%ebx
	ret
.Lgs_default:
	call	abort@PLT
	.size	gcc_switch, .-gcc_switch

	.p2align 4
	.globl	gcc_fused_switch
	.type	gcc_fused_switch, @function
gcc_fused_switch:
	call	__x86.get_pc_thunk.ax
	addl	$_GLOBAL_OFFSET_TABLE_, %eax
	movl	4(%esp), %edx
	cmpl	$3, %edx
	ja	.Lgf_default
	addl	.Lgf_table@GOTOFF(%eax,%edx,4), %eax
gcc_fused_switch_dispatch:
	jmp	*%eax
gcc_fused_switch_case0:
	movl	8(%esp), %eax
	addl	$3, %eax
	ret
gcc_fused_switch_case1:
	movl	8(%esp), %eax
	imull	$7, %eax, %eax
	ret
gcc_fused_switch_case2:
	movl	8(%esp), %eax
	subl	$11, %eax
	ret
gcc_fused_switch_case3:
	movl	8(%esp), %eax
	xorl	$85, %eax
	ret
.Lgf_default:
	xorl	%eax, %eax
	ret
	.size	gcc_fused_switch, .-gcc_fused_switch

	.p2align 4
	.globl	clang_switch
	.type	clang_switch, @function
clang_switch:
	calll	.Lcs_pc
.Lcs_pc:
	popl	%ecx
.Lcs_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lcs_got-.Lcs_pc), %ecx
	movl	4(%esp), %edx
	cmpl	$3, %edx
	ja	.Lcs_default
	movl	8(%esp), %eax
	addl	.Lcs_table@GOTOFF(%ecx,%edx,4), %ecx
clang_switch_dispatch:
	jmpl	*%ecx
clang_switch_case0:
	addl	$3, %eax
	retl
clang_switch_case1:
	shll	$2, %eax
	retl
clang_switch_case2:
	negl	%eax
	retl
clang_switch_case3:
	notl	%eax
	retl
.Lcs_default:
	xorl	%eax, %eax
	retl
	.size	clang_switch, .-clang_switch

	.p2align 4
	.globl	saved_got_switch
	.type	saved_got_switch, @function
saved_got_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	testl	$1, 12(%esp)
	je	.Lsg_merge
	call	note_entry
.Lsg_merge:
	movl	8(%esp), %eax
	cmpl	$3, %eax
	ja	.Lsg_default
	movl	.Lsg_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
saved_got_switch_dispatch:
	jmp	*%eax
.irp n,0,1,2,3
saved_got_switch_case\n:
	movl	$\n+100, %eax
	popl	%ebx
	ret
.endr
.Lsg_default:
	xorl	%eax, %eax
	popl	%ebx
	ret
	.size	saved_got_switch, .-saved_got_switch

	.p2align 4
	.type	note_entry, @function
note_entry:
	ret
	.size	note_entry, .-note_entry

	.p2align 4
	.globl	offset_anchor_switch
	.type	offset_anchor_switch, @function
offset_anchor_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_+4, %ebx
	movl	8(%esp), %eax
	cmpl	$3, %eax
	ja	.Loa_default
	movl	.Loa_table@GOTOFF-4(%ebx,%eax,4), %eax
	addl	%ebx, %eax
offset_anchor_switch_dispatch:
	jmp	*%eax
.irp n,0,1,2,3
offset_anchor_switch_case\n:
	nopl	0(%eax)
	movl	$\n, %eax
	popl	%ebx
	ret
.endr
.Loa_default:
	xorl	%eax, %eax
	popl	%ebx
	ret
	.size	offset_anchor_switch, .-offset_anchor_switch

	.p2align 4
	.globl	clobbered_anchor_switch
	.type	clobbered_anchor_switch, @function
clobbered_anchor_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	movl	8(%esp), %eax
	testl	$1, 12(%esp)
	je	.Lca_keep
	movl	16(%esp), %ebx
.Lca_keep:
	cmpl	$3, %eax
	ja	.Lca_default
	movl	.Lca_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
clobbered_anchor_switch_dispatch:
	jmp	*%eax
.irp n,0,1,2,3
clobbered_anchor_switch_case\n:
	movl	$\n, %eax
	popl	%ebx
	ret
.endr
.Lca_default:
	xorl	%eax, %eax
	popl	%ebx
	ret
	.size	clobbered_anchor_switch, .-clobbered_anchor_switch

	.section	.rodata
	.p2align 2
.Lgs_table:
.irp n,0,1,2,3,4
	.long	gcc_switch_case\n@GOTOFF
.endr
.Lgf_table:
.irp n,0,1,2,3
	.long	gcc_fused_switch_case\n@GOTOFF
.endr
.Lcs_table:
.irp n,0,1,2,3
	.long	clang_switch_case\n@GOTOFF
.endr
.Lsg_table:
.irp n,0,1,2,3
	.long	saved_got_switch_case\n@GOTOFF
.endr
.Loa_table:
.irp n,0,1,2,3
	.long	offset_anchor_switch_case\n@GOTOFF
.endr
.Lca_table:
.irp n,0,1,2,3
	.long	clobbered_anchor_switch_case\n@GOTOFF
.endr

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

	.section	.note.GNU-stack,"",@progbits

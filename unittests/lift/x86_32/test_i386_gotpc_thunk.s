// i386 PIC switches as GCC compiles them into an object file: the GOT address
// comes from a call to a get-PC thunk and an `addl $_GLOBAL_OFFSET_TABLE_`
// whose R_386_GOTPC field expects the call's return address.
//   jt_i386_thunk_switch          gcc -m32 -O2 -fPIC: `mov tbl@GOTOFF(...)`, add
//   jt_i386_thunk_fused_switch    gcc's fused `add tbl@GOTOFF(%eax,%edx,4)`
//   jt_i386_thunk_other_register  the add names a register the thunk did not
//                                 load, so no GOT-base model may be published

	.text
	.p2align 4
	.globl	jt_i386_thunk_switch
	.type	jt_i386_thunk_switch, @function
jt_i386_thunk_switch:
	pushl	%ebx
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %ebx
	movl	8(%esp), %eax
	cmpl	$4, %eax
	ja	.Lts_default
	movl	.Lts_table@GOTOFF(%ebx,%eax,4), %eax
	addl	%ebx, %eax
	.globl	jt_i386_thunk_switch_branch
jt_i386_thunk_switch_branch:
	jmp	*%eax
.irp n,0,1,2,3,4
jt_i386_thunk_switch_case\n:
	movl	$6200+\n, %eax
	popl	%ebx
	ret
.endr
.Lts_default:
	movl	$-1, %eax
	popl	%ebx
	ret
	.size	jt_i386_thunk_switch, .-jt_i386_thunk_switch

	.p2align 4
	.globl	jt_i386_thunk_fused_switch
	.type	jt_i386_thunk_fused_switch, @function
jt_i386_thunk_fused_switch:
	call	__x86.get_pc_thunk.ax
	addl	$_GLOBAL_OFFSET_TABLE_, %eax
	movl	4(%esp), %edx
	cmpl	$3, %edx
	ja	.Ltf_default
	addl	.Ltf_table@GOTOFF(%eax,%edx,4), %eax
	.globl	jt_i386_thunk_fused_switch_branch
jt_i386_thunk_fused_switch_branch:
	jmp	*%eax
.irp n,0,1,2,3
jt_i386_thunk_fused_switch_case\n:
	movl	$6300+\n, %eax
	ret
.endr
.Ltf_default:
	movl	$-1, %eax
	ret
	.size	jt_i386_thunk_fused_switch, .-jt_i386_thunk_fused_switch

	.p2align 4
	.globl	jt_i386_thunk_other_register
	.type	jt_i386_thunk_other_register, @function
jt_i386_thunk_other_register:
	pushl	%ebx
	movl	12(%esp), %eax
	call	__x86.get_pc_thunk.bx
	addl	$_GLOBAL_OFFSET_TABLE_, %eax
	movl	8(%esp), %ecx
	cmpl	$3, %ecx
	ja	.Lto_default
	movl	.Lto_table@GOTOFF(%eax,%ecx,4), %edx
	addl	%eax, %edx
	.globl	jt_i386_thunk_other_register_branch
jt_i386_thunk_other_register_branch:
	jmp	*%edx
.irp n,0,1,2,3
jt_i386_thunk_other_register_case\n:
	movl	$6400+\n, %eax
	popl	%ebx
	ret
.endr
.Lto_default:
	movl	$-1, %eax
	popl	%ebx
	ret
	.size	jt_i386_thunk_other_register, .-jt_i386_thunk_other_register

	.section	.rodata
	.p2align 2
.Lts_table:
.irp n,0,1,2,3,4
	.long	jt_i386_thunk_switch_case\n@GOTOFF
.endr
.Ltf_table:
.irp n,0,1,2,3
	.long	jt_i386_thunk_fused_switch_case\n@GOTOFF
.endr
.Lto_table:
.irp n,0,1,2,3
	.long	jt_i386_thunk_other_register_case\n@GOTOFF
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

// GCC 15.2 `g++ -O2 -fPIC -S` of
//   struct Guard { Guard(int); ~Guard(); int v; };
//   void use(int);
//   extern "C" int gcc_cold_guard(int n) {
//     Guard g(n);
//     use(n + g.v);
//     return n + 1;
//   }
// Hot/cold partitioning moves the cleanup that destroys `g` before
// _Unwind_Resume to `gcc_cold_guard.cold` in .text.unlikely, with an FDE of
// its own that begins inside gcc_cold_guard's frame (CFA rsp+48, rbx and rbp
// saved).  The landing pad in gcc_cold_guard jumps there.
	.text
	.section	.text.unlikely,"ax",@progbits
.LCOLDB0:
	.text
.LHOTB0:
	.p2align 4
	.globl	gcc_cold_guard
	.type	gcc_cold_guard, @function
gcc_cold_guard:
.LFB0:
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDA0
	endbr64
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	pushq	%rbx
	.cfi_def_cfa_offset 24
	.cfi_offset 3, -24
	subq	$24, %rsp
	.cfi_def_cfa_offset 48
	movq	%fs:40, %rbx
	movq	%rbx, 8(%rsp)
	movl	%edi, %ebx
	leaq	4(%rsp), %rbp
	movq	%rbp, %rdi
	movl	%ebx, %esi
.LEHB0:
	call	_ZN5GuardC1Ei@PLT
.LEHE0:
	movl	4(%rsp), %edi
	addl	%ebx, %edi
.LEHB1:
	call	_Z3usei@PLT
.LEHE1:
	movq	%rbp, %rdi
	call	_ZN5GuardD1Ev@PLT
	leal	1(%rbx), %eax
	movq	8(%rsp), %rdx
	subq	%fs:40, %rdx
	jne	.L9
	addq	$24, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 24
	popq	%rbx
	.cfi_def_cfa_offset 16
	popq	%rbp
	.cfi_def_cfa_offset 8
	ret
.L9:
	.cfi_restore_state
	call	__stack_chk_fail@PLT
.L5:
	endbr64
	movq	%rax, %rbx
	jmp	.L2
	.section	.gcc_except_table,"a",@progbits
.LLSDA0:
	.byte	0xff
	.byte	0xff
	.byte	0x1
	.uleb128 .LLSDACSE0-.LLSDACSB0
.LLSDACSB0:
	.uleb128 .LEHB0-.LFB0
	.uleb128 .LEHE0-.LEHB0
	.uleb128 0
	.uleb128 0
	.uleb128 .LEHB1-.LFB0
	.uleb128 .LEHE1-.LEHB1
	.uleb128 .L5-.LFB0
	.uleb128 0
.LLSDACSE0:
	.text
	.cfi_endproc
	.section	.text.unlikely
	.cfi_startproc
	.cfi_personality 0x9b,DW.ref.__gxx_personality_v0
	.cfi_lsda 0x1b,.LLSDAC0
	.type	gcc_cold_guard.cold, @function
gcc_cold_guard.cold:
.LFSB0:
.L2:
	.cfi_def_cfa_offset 48
	.cfi_offset 3, -24
	.cfi_offset 6, -16
	movq	%rbp, %rdi
	call	_ZN5GuardD1Ev@PLT
	movq	8(%rsp), %rax
	subq	%fs:40, %rax
	jne	.L10
	movq	%rbx, %rdi
.LEHB2:
	call	_Unwind_Resume@PLT
.LEHE2:
.L10:
	call	__stack_chk_fail@PLT
	.cfi_endproc
.LFE0:
	.section	.gcc_except_table
.LLSDAC0:
	.byte	0xff
	.byte	0xff
	.byte	0x1
	.uleb128 .LLSDACSEC0-.LLSDACSBC0
.LLSDACSBC0:
	.uleb128 .LEHB2-.LCOLDB0
	.uleb128 .LEHE2-.LEHB2
	.uleb128 0
	.uleb128 0
.LLSDACSEC0:
	.section	.text.unlikely
	.text
	.size	gcc_cold_guard, .-gcc_cold_guard
	.section	.text.unlikely
	.size	gcc_cold_guard.cold, .-gcc_cold_guard.cold
.LCOLDE0:
	.text
.LHOTE0:
	.hidden	DW.ref.__gxx_personality_v0
	.weak	DW.ref.__gxx_personality_v0
	.section	.data.rel.local.DW.ref.__gxx_personality_v0,"awG",@progbits,DW.ref.__gxx_personality_v0,comdat
	.align 8
	.type	DW.ref.__gxx_personality_v0, @object
	.size	DW.ref.__gxx_personality_v0, 8
DW.ref.__gxx_personality_v0:
	.quad	__gxx_personality_v0
	.globl	__gxx_personality_v0
	.section	.note.GNU-stack,"",@progbits
	.section	.note.gnu.property,"a"
	.align 8
	.long	1f - 0f
	.long	4f - 1f
	.long	5
0:
	.string	"GNU"
1:
	.align 8
	.long	0xc0000002
	.long	3f - 2f
2:
	.long	0x3
3:
	.align 8
4:

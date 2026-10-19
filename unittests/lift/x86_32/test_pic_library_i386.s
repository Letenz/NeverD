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
//   static const int table[8] = {3, 1, 4, 1, 5, 9, 2, 6};
//   int gcc_lookup(int i) { return table[i & 7]; }
// `gcc_lookup` adds the index to the GOT register its thunk loads,
// `clang_lookup` to the address its call/pop leaves, before the GOTOFF
// displacement of `table`.
//   void clang_set_factor(int v) { factor = v; }
// returns with the GOT address its call/pop left in %eax.
//   void clang_call_handler(void (*f)(void)) { f(); factor = 1; }
// calls through its argument with no stack argument, in a realigned frame,
// after its call/pop.
//   static int (*clang_hook)(void) = clang_scale_three;
//   int clang_spill_call(int a, int b) { return clang_hook(); }
// built at -O0 spills both arguments and the GOT address to its frame and
// calls through `clang_hook` with no argument.
// `clang_pass_result`, from -O0 code, stores the result of a call through
// a table at 4(%esp) and calls the next entry, leaving 0(%esp) unwritten,
// as android_x86_server's `openssl_api_10_t::library_init` does.
//   int clang_call_keep_got(void (*f)(void)) { f(); return factor; }
// built at -O0 spills the GOT address to 4(%esp) before calling `f` and
// reloads it to read `factor`.
//   void clang_set_factor_o0(int v) { factor = v; }
// built at -O0 spills the GOT address and returns with its reload in %eax.
// `clang_ui_value` is the IDA SDK's `vmsg` at -O0: the pointer `clang_hook`
// returns a one-word struct in memory and pops its hidden pointer, so
//   int clang_ui_value(void) { return clang_hook(23).i; }
// takes the popped word back with `subl $4, %esp` and reads the result.  A
// stack model that does not know the callee pops reads the GOT's spill
// there.  `clang_ui_caller` stores what it returns, calling it through its
// PLT entry, which the loader starts at the routine itself:
//   void clang_ui_caller(void) { factor = clang_ui_value(); }
// `clang_forward_factor`, built with -fno-optimize-sibling-calls, returns
// with what `clang_set_factor`, called through its PLT entry, left in %eax:
//   void clang_forward_factor(int v) { clang_set_factor(v); }
// `clang_maybe_scale` returns with %eax holding what `clang_scale_three`
// returned on one path and the GOT address on the other:
//   void clang_maybe_scale(int v) { if (v) clang_scale_three(); else factor = 0; }
// `clang_nothrow_call`, as clang -O1 -fexceptions builds
//   extern "C" void clang_nothrow_call(int v) noexcept { may_throw(v); }
// has a landing pad that passes the exception object in %eax on the stack:
// `movl %eax, (%esp); calll __clang_call_terminate`.
// `clang_catch_what`, as clang -O0 -fomit-frame-pointer -fexceptions builds
//   struct Error { virtual ~Error(); virtual const char *what() const noexcept; };
//   extern "C" const char *clang_catch_what(int v) {
//     try { may_throw(v); } catch (const Error &e) { return e.what(); }
//     return nullptr;
//   }
// keeps its locals at ESP offsets, and its landing pad passes the exception
// object, then the caught one, on the stack.

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

	.p2align 4
	.globl	gcc_lookup
	.type	gcc_lookup, @function
gcc_lookup:
	.cfi_startproc
	call	__x86.get_pc_thunk.dx
	addl	$_GLOBAL_OFFSET_TABLE_, %edx
	movl	4(%esp), %eax
	andl	$7, %eax
	movl	table@GOTOFF(%edx,%eax,4), %eax
	ret
	.cfi_endproc
	.size	gcc_lookup, .-gcc_lookup

	.p2align 4
	.globl	clang_lookup
	.type	clang_lookup, @function
clang_lookup:
	.cfi_startproc
	calll	.Llookup_pc
	.cfi_adjust_cfa_offset 4
.Llookup_pc:
	popl	%ecx
	.cfi_adjust_cfa_offset -4
.Llookup_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Llookup_got-.Llookup_pc), %ecx
	movl	4(%esp), %eax
	andl	$7, %eax
	movl	table@GOTOFF(%ecx,%eax,4), %eax
	retl
	.cfi_endproc
	.size	clang_lookup, .-clang_lookup

	.p2align 4
	.globl	clang_set_factor
	.type	clang_set_factor, @function
clang_set_factor:
	.cfi_startproc
	calll	.Lset_factor_pc
	.cfi_adjust_cfa_offset 4
.Lset_factor_pc:
	popl	%eax
	.cfi_adjust_cfa_offset -4
.Lset_factor_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lset_factor_got-.Lset_factor_pc), %eax
	movl	4(%esp), %ecx
	movl	%ecx, factor@GOTOFF(%eax)
	retl
	.cfi_endproc
	.size	clang_set_factor, .-clang_set_factor

	.p2align 4
	.globl	clang_call_handler
	.type	clang_call_handler, @function
clang_call_handler:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	pushl	%esi
	.cfi_offset %esi, -12
	andl	$-16, %esp
	subl	$16, %esp
	calll	clang_call_handler_pop
clang_call_handler_pop:
	popl	%esi
.Lcall_handler_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lcall_handler_got-clang_call_handler_pop), %esi
	calll	*8(%ebp)
	movl	$1, factor@GOTOFF(%esi)
	leal	-4(%ebp), %esp
	popl	%esi
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_call_handler, .-clang_call_handler

	.p2align 4
	.type	clang_scale_three, @function
clang_scale_three:
	movl	$3, %eax
	retl
	.size	clang_scale_three, .-clang_scale_three

	.p2align 4
	.globl	clang_spill_call
	.type	clang_spill_call, @function
clang_spill_call:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	pushl	%esi
	.cfi_offset %esi, -12
	andl	$-16, %esp
	subl	$32, %esp
	calll	.Lspill_call_pc
.Lspill_call_pc:
	popl	%eax
.Lspill_call_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lspill_call_got-.Lspill_call_pc), %eax
	movl	12(%ebp), %ecx
	movl	8(%ebp), %edx
	leal	clang_hook@GOTOFF(%eax), %esi
	movl	%eax, 28(%esp)
	movl	%ecx, 24(%esp)
	movl	%edx, 20(%esp)
	calll	*(%esi)
	leal	-4(%ebp), %esp
	popl	%esi
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_spill_call, .-clang_spill_call

	.p2align 4
	.globl	clang_pass_result
	.type	clang_pass_result, @function
clang_pass_result:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	subl	$24, %esp
	movl	8(%ebp), %eax
	movl	%eax, 8(%esp)
	calll	*(%eax)
	movl	8(%esp), %ecx
	movl	%eax, 4(%esp)
	calll	*4(%ecx)
	movl	%ebp, %esp
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_pass_result, .-clang_pass_result

	.p2align 4
	.globl	clang_call_keep_got
	.type	clang_call_keep_got, @function
clang_call_keep_got:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	pushl	%esi
	.cfi_offset %esi, -12
	andl	$-16, %esp
	subl	$16, %esp
	calll	.Lkeep_got_pc
.Lkeep_got_pc:
	popl	%eax
.Lkeep_got_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lkeep_got_got-.Lkeep_got_pc), %eax
	movl	%eax, 4(%esp)
	movl	8(%ebp), %eax
	calll	*%eax
	movl	4(%esp), %ecx
	movl	factor@GOTOFF(%ecx), %eax
	leal	-4(%ebp), %esp
	popl	%esi
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_call_keep_got, .-clang_call_keep_got

	.p2align 4
	.globl	clang_set_factor_o0
	.type	clang_set_factor_o0, @function
clang_set_factor_o0:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	subl	$8, %esp
	calll	.Lset_factor_o0_pc
.Lset_factor_o0_pc:
	popl	%eax
.Lset_factor_o0_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lset_factor_o0_got-.Lset_factor_o0_pc), %eax
	movl	%eax, 4(%esp)
	movl	8(%ebp), %ecx
	movl	4(%esp), %eax
	movl	%ecx, factor@GOTOFF(%eax)
	addl	$8, %esp
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_set_factor_o0, .-clang_set_factor_o0

	.p2align 4
	.globl	clang_ui_value
	.type	clang_ui_value, @function
clang_ui_value:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset %ebp, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register %ebp
	pushl	%ebx
	pushl	%esi
	.cfi_offset %esi, -16
	.cfi_offset %ebx, -12
	andl	$-16, %esp
	subl	$32, %esp
	calll	.Lui_value_pc
.Lui_value_pc:
	popl	%eax
.Lui_value_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lui_value_got-.Lui_value_pc), %eax
	movl	clang_hook@GOTOFF(%eax), %esi
	movl	%eax, 20(%esp)
	leal	24(%esp), %ecx
	movl	%ecx, (%esp)
	movl	$23, 4(%esp)
	movl	20(%esp), %ebx
	calll	*%esi
	subl	$4, %esp
	movl	24(%esp), %eax
	leal	-8(%ebp), %esp
	popl	%esi
	popl	%ebx
	popl	%ebp
	.cfi_def_cfa %esp, 4
	retl
	.cfi_endproc
	.size	clang_ui_value, .-clang_ui_value

	.p2align 4
	.globl	clang_ui_caller
	.type	clang_ui_caller, @function
clang_ui_caller:
	.cfi_startproc
	pushl	%ebx
	.cfi_def_cfa_offset 8
	.cfi_offset %ebx, -8
	subl	$8, %esp
	.cfi_def_cfa_offset 16
	calll	.Lui_caller_pc
.Lui_caller_pc:
	popl	%ebx
.Lui_caller_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lui_caller_got-.Lui_caller_pc), %ebx
	calll	clang_ui_value@PLT
	movl	%eax, factor@GOTOFF(%ebx)
	addl	$8, %esp
	.cfi_def_cfa_offset 8
	popl	%ebx
	.cfi_def_cfa_offset 4
	retl
	.cfi_endproc
	.size	clang_ui_caller, .-clang_ui_caller

	.p2align 4
	.globl	clang_forward_factor
	.type	clang_forward_factor, @function
clang_forward_factor:
	.cfi_startproc
	pushl	%ebx
	.cfi_def_cfa_offset 8
	.cfi_offset %ebx, -8
	subl	$8, %esp
	.cfi_def_cfa_offset 16
	calll	.Lforward_factor_pc
.Lforward_factor_pc:
	popl	%ebx
.Lforward_factor_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lforward_factor_got-.Lforward_factor_pc), %ebx
	movl	16(%esp), %eax
	movl	%eax, (%esp)
	calll	clang_set_factor@PLT
	addl	$8, %esp
	.cfi_def_cfa_offset 8
	popl	%ebx
	.cfi_def_cfa_offset 4
	retl
	.cfi_endproc
	.size	clang_forward_factor, .-clang_forward_factor

	.p2align 4
	.globl	clang_maybe_scale
	.type	clang_maybe_scale, @function
clang_maybe_scale:
	.cfi_startproc
	subl	$12, %esp
	.cfi_def_cfa_offset 16
	cmpl	$0, 16(%esp)
	je	.Lmaybe_scale_zero
	calll	clang_scale_three
	jmp	.Lmaybe_scale_done
.Lmaybe_scale_zero:
	calll	.Lmaybe_scale_pc
.Lmaybe_scale_pc:
	popl	%eax
.Lmaybe_scale_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lmaybe_scale_got-.Lmaybe_scale_pc), %eax
	movl	$0, factor@GOTOFF(%eax)
.Lmaybe_scale_done:
	addl	$12, %esp
	.cfi_def_cfa_offset 4
	retl
	.cfi_endproc
	.size	clang_maybe_scale, .-clang_maybe_scale

	.p2align 4
	.globl	clang_nothrow_call
	.type	clang_nothrow_call, @function
clang_nothrow_call:
.Lnothrow_begin:
	.cfi_startproc
	.cfi_personality 155, DW.ref.__gxx_personality_v0
	.cfi_lsda 27, .Lnothrow_lsda
	pushl	%ebx
	.cfi_def_cfa_offset 8
	subl	$8, %esp
	.cfi_def_cfa_offset 16
	.cfi_offset %ebx, -8
	calll	.Lnothrow_pc
	.cfi_adjust_cfa_offset 4
.Lnothrow_pc:
	popl	%ebx
	.cfi_adjust_cfa_offset -4
.Lnothrow_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lnothrow_got-.Lnothrow_pc), %ebx
	movl	16(%esp), %eax
.Lnothrow_try_begin:
	movl	%eax, (%esp)
	calll	_Z9may_throwi@PLT
.Lnothrow_try_end:
	addl	$8, %esp
	.cfi_def_cfa_offset 8
	popl	%ebx
	.cfi_def_cfa_offset 4
	retl
.Lnothrow_pad:
	.cfi_def_cfa_offset 16
	movl	%eax, (%esp)
	calll	__clang_call_terminate
.Lnothrow_end:
	.size	clang_nothrow_call, .Lnothrow_end-clang_nothrow_call
	.cfi_endproc

	.section	.gcc_except_table,"a",@progbits
	.p2align	2, 0x0
.Lnothrow_lsda:
	.byte	255
	.byte	155
	.uleb128 .Lnothrow_ttbase-.Lnothrow_ttbaseref
.Lnothrow_ttbaseref:
	.byte	1
	.uleb128 .Lnothrow_cst_end-.Lnothrow_cst_begin
.Lnothrow_cst_begin:
	.uleb128 .Lnothrow_try_begin-.Lnothrow_begin
	.uleb128 .Lnothrow_try_end-.Lnothrow_try_begin
	.uleb128 .Lnothrow_pad-.Lnothrow_begin
	.byte	1
.Lnothrow_cst_end:
	.byte	1
	.byte	0
	.p2align	2, 0x0
	.long	0
.Lnothrow_ttbase:

	.section	.text.__clang_call_terminate,"axG",@progbits,__clang_call_terminate,comdat
	.hidden	__clang_call_terminate
	.weak	__clang_call_terminate
	.p2align 4
	.type	__clang_call_terminate, @function
__clang_call_terminate:
	pushl	%ebx
	subl	$8, %esp
	calll	.Lterminate_pc
.Lterminate_pc:
	popl	%ebx
.Lterminate_got:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lterminate_got-.Lterminate_pc), %ebx
	movl	16(%esp), %eax
	movl	%eax, (%esp)
	calll	__cxa_begin_catch@PLT
	calll	_ZSt9terminatev@PLT
.Lterminate_end:
	.size	__clang_call_terminate, .Lterminate_end-__clang_call_terminate

	.globl	clang_catch_what
	.p2align 4
	.type	clang_catch_what,@function
clang_catch_what:
.Lwhat_begin:
	.cfi_startproc
	.cfi_personality 155, DW.ref.__gxx_personality_v0
	.cfi_lsda 27, .Lwhat_lsda
	pushl	%ebx
	.cfi_def_cfa_offset 8
	subl	$24, %esp
	.cfi_def_cfa_offset 32
	.cfi_offset %ebx, -8
	calll	.Lwhat_pb
	.cfi_adjust_cfa_offset 4
.Lwhat_pb:
	popl	%ebx
	.cfi_adjust_cfa_offset -4
.Lwhat_tmp3:
	addl	$_GLOBAL_OFFSET_TABLE_+(.Lwhat_tmp3-.Lwhat_pb), %ebx
	movl	%ebx, 4(%esp)
	movl	32(%esp), %eax
	movl	32(%esp), %ecx
.Lwhat_tmp0:
	movl	%esp, %eax
	movl	%ecx, (%eax)
	calll	_Z9may_throwi@PLT
.Lwhat_tmp1:
	jmp	.Lwhat_bb1
.Lwhat_bb1:
	jmp	.Lwhat_bb5
.Lwhat_bb2:
.Lwhat_tmp2:
	movl	%eax, 16(%esp)
	movl	%edx, 12(%esp)
	movl	12(%esp), %eax
	movl	$1, %ecx
	cmpl	%ecx, %eax
	jne	.Lwhat_bb7
	movl	4(%esp), %ebx
	movl	16(%esp), %eax
	movl	%eax, (%esp)
	calll	__cxa_begin_catch@PLT
	movl	4(%esp), %ebx
	movl	%eax, 8(%esp)
	movl	8(%esp), %ecx
	movl	(%ecx), %eax
	movl	%ecx, (%esp)
	calll	*8(%eax)
	movl	4(%esp), %ebx
	movl	%eax, 20(%esp)
	calll	__cxa_end_catch@PLT
	jmp	.Lwhat_bb6
.Lwhat_bb5:
	movl	$0, 20(%esp)
.Lwhat_bb6:
	movl	20(%esp), %eax
	addl	$24, %esp
	.cfi_def_cfa_offset 8
	popl	%ebx
	.cfi_def_cfa_offset 4
	retl
.Lwhat_bb7:
	.cfi_def_cfa_offset 32
	movl	4(%esp), %ebx
	movl	16(%esp), %eax
	movl	%eax, (%esp)
	calll	_Unwind_Resume@PLT
.Lwhat_end:
	.size	clang_catch_what, .Lwhat_end-clang_catch_what
	.cfi_endproc
	.section	.gcc_except_table,"a",@progbits
	.p2align	2, 0x0
.Lwhat_table:
.Lwhat_lsda:
	.byte	255
	.byte	155
	.uleb128 .Lwhat_ttbase-.Lwhat_ttbaseref
.Lwhat_ttbaseref:
	.byte	1
	.uleb128 .Lwhat_cst_end-.Lwhat_cst_begin
.Lwhat_cst_begin:
	.uleb128 .Lwhat_tmp0-.Lwhat_begin
	.uleb128 .Lwhat_tmp1-.Lwhat_tmp0
	.uleb128 .Lwhat_tmp2-.Lwhat_begin
	.byte	1
	.uleb128 .Lwhat_tmp1-.Lwhat_begin
	.uleb128 .Lwhat_end-.Lwhat_tmp1
	.byte	0
	.byte	0
.Lwhat_cst_end:
	.byte	1
	.byte	0
	.p2align	2, 0x0
.Lwhat_tmp4:
	.long	.Lwhat_ZTI5Error_stub-.Lwhat_tmp4
.Lwhat_ttbase:
	.p2align	2, 0x0
	.data
	.p2align	2, 0x0
.Lwhat_ZTI5Error_stub:
	.long	_ZTI5Error


	.hidden	DW.ref.__gxx_personality_v0
	.weak	DW.ref.__gxx_personality_v0
	.section	.data.DW.ref.__gxx_personality_v0,"awG",@progbits,DW.ref.__gxx_personality_v0,comdat
	.p2align	2, 0x0
	.type	DW.ref.__gxx_personality_v0, @object
	.size	DW.ref.__gxx_personality_v0, 4
DW.ref.__gxx_personality_v0:
	.long	__gxx_personality_v0

	.text

	.section	.rodata
	.p2align 2
	.type	table, @object
	.size	table, 32
table:
	.long	3, 1, 4, 1, 5, 9, 2, 6

	.data
	.p2align 2
	.type	factor, @object
	.size	factor, 4
factor:
	.long	3

	.p2align 2
	.type	clang_hook, @object
	.size	clang_hook, 4
clang_hook:
	.long	clang_scale_three

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

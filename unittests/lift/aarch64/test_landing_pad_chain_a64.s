// Landing pads that one jump and the unwinder both enter, in the shape rustc
// gives std::panicking::panic_with_hook.
//
// The unwinder enters .Lpad_jump when first_call throws, and .Lpad_jump only
// branches to .Lpad_guard.  It also enters .Lpad_guard directly when
// fail_call throws.  .Lpad_guard runs a cleanup that never returns and ends
// in `brk #1`.  .Lpad_cleanup, the pad of second_call, ends in
// _Unwind_Resume, so ordinary code never falls into .Lpad_guard.
//
// pad_flag and pad_copy each have one pad after a call that never returns,
// as std::panicking::panic_handler's closure does.  The pad starts with a
// load that only the next statement reads: a comparison in pad_flag, a store
// in pad_copy.  Folding that load into its reader removes the statement the
// pad's label sits on.
	.text
	.globl	pad_chain
	.p2align	2
	.type	pad_chain,@function
pad_chain:
.Lfunc_begin0:
	.cfi_startproc
	.cfi_personality 156, DW.ref.__gxx_personality_v0
	.cfi_lsda 28, .Lexception0
	stp	x29, x30, [sp, #-32]!
	.cfi_def_cfa_offset 32
	str	x19, [sp, #16]
	mov	x29, sp
	.cfi_def_cfa w29, 32
	.cfi_offset w19, -16
	.cfi_offset w30, -24
	.cfi_offset w29, -32
	cbz	w0, .Lfail
	cmp	w0, #1
	b.eq	.Lsecond
.Ltmp0:
	bl	first_call
.Ltmp1:
	b	.Lreturn
.Lsecond:
.Ltmp2:
	bl	second_call
.Ltmp3:
.Lreturn:
	ldr	x19, [sp, #16]
	ldp	x29, x30, [sp], #32
	ret
.Lpad_jump:
	b	.Lpad_guard
.Lpad_cleanup:
	mov	x19, x0
	bl	cleanup
	mov	x0, x19
	bl	_Unwind_Resume
.Lfail:
.Ltmp4:
	bl	fail_call
.Ltmp5:
	brk	#0x1
.Lpad_guard:
	bl	guard_drop
	brk	#0x1
.Lfunc_end0:
	.size	pad_chain, .Lfunc_end0-pad_chain
	.cfi_endproc

	.section	.gcc_except_table,"a",@progbits
	.p2align	2
.Lexception0:
	.byte	255                     // @LPStart encoding: omitted
	.byte	255                     // @TType encoding: omitted
	.byte	1                       // call-site encoding: uleb128
	.uleb128 .Lcst_end0-.Lcst_begin0
.Lcst_begin0:
	.uleb128 .Ltmp0-.Lfunc_begin0   // first_call
	.uleb128 .Ltmp1-.Ltmp0
	.uleb128 .Lpad_jump-.Lfunc_begin0
	.byte	0                       // cleanup
	.uleb128 .Ltmp2-.Lfunc_begin0   // second_call
	.uleb128 .Ltmp3-.Ltmp2
	.uleb128 .Lpad_cleanup-.Lfunc_begin0
	.byte	0                       // cleanup
	.uleb128 .Lpad_cleanup-.Lfunc_begin0 // the cleanup pad's calls
	.uleb128 .Lfail-.Lpad_cleanup
	.byte	0                       // no landing pad
	.byte	0
	.uleb128 .Ltmp4-.Lfunc_begin0   // fail_call
	.uleb128 .Ltmp5-.Ltmp4
	.uleb128 .Lpad_guard-.Lfunc_begin0
	.byte	0                       // cleanup
	.uleb128 .Lpad_guard-.Lfunc_begin0 // guard_drop
	.uleb128 .Lfunc_end0-.Lpad_guard
	.byte	0                       // no landing pad
	.byte	0
.Lcst_end0:
	.p2align	2

	.text
	.globl	pad_flag
	.p2align	2
	.type	pad_flag,@function
pad_flag:
.Lfunc_begin1:
	.cfi_startproc
	.cfi_personality 156, DW.ref.__gxx_personality_v0
	.cfi_lsda 28, .Lexception1
	stp	x29, x30, [sp, #-48]!
	.cfi_def_cfa_offset 48
	str	x19, [sp, #16]
	mov	x29, sp
	.cfi_def_cfa w29, 48
	.cfi_offset w19, -32
	.cfi_offset w30, -40
	.cfi_offset w29, -48
	str	xzr, [sp, #32]
	add	x0, sp, #32
.Ltmp10:
	bl	fill_flag
.Ltmp11:
	add	x0, sp, #32
	bl	abort
.Lpad_flag:
	ldr	x1, [sp, #32]
	cmp	x1, #1
	b.lt	.Lresume_flag
	mov	x19, x0
	ldr	x0, [sp, #40]
	mov	x1, #8
	bl	release
	mov	x0, x19
.Lresume_flag:
	bl	_Unwind_Resume
.Lfunc_end1:
	.size	pad_flag, .Lfunc_end1-pad_flag
	.cfi_endproc

	.globl	pad_copy
	.p2align	2
	.type	pad_copy,@function
pad_copy:
.Lfunc_begin2:
	.cfi_startproc
	.cfi_personality 156, DW.ref.__gxx_personality_v0
	.cfi_lsda 28, .Lexception2
	stp	x29, x30, [sp, #-48]!
	.cfi_def_cfa_offset 48
	mov	x29, sp
	.cfi_def_cfa w29, 48
	.cfi_offset w30, -40
	.cfi_offset w29, -48
	str	xzr, [sp, #32]
	add	x0, sp, #32
.Ltmp20:
	bl	fill_flag
.Ltmp21:
	add	x0, sp, #32
	bl	abort
.Lpad_copy:
	ldr	x8, [sp, #40]
	str	x8, [sp, #32]
	bl	_Unwind_Resume
.Lfunc_end2:
	.size	pad_copy, .Lfunc_end2-pad_copy
	.cfi_endproc

	.section	.gcc_except_table,"a",@progbits
	.p2align	2
.Lexception1:
	.byte	255                     // @LPStart encoding: omitted
	.byte	255                     // @TType encoding: omitted
	.byte	1                       // call-site encoding: uleb128
	.uleb128 .Lcst_end1-.Lcst_begin1
.Lcst_begin1:
	.uleb128 .Ltmp10-.Lfunc_begin1  // fill_flag
	.uleb128 .Ltmp11-.Ltmp10
	.uleb128 .Lpad_flag-.Lfunc_begin1
	.byte	0                       // cleanup
	.uleb128 .Ltmp11-.Lfunc_begin1  // abort and the pad's calls
	.uleb128 .Lfunc_end1-.Ltmp11
	.byte	0                       // no landing pad
	.byte	0
.Lcst_end1:
	.p2align	2
.Lexception2:
	.byte	255                     // @LPStart encoding: omitted
	.byte	255                     // @TType encoding: omitted
	.byte	1                       // call-site encoding: uleb128
	.uleb128 .Lcst_end2-.Lcst_begin2
.Lcst_begin2:
	.uleb128 .Ltmp20-.Lfunc_begin2  // fill_flag
	.uleb128 .Ltmp21-.Ltmp20
	.uleb128 .Lpad_copy-.Lfunc_begin2
	.byte	0                       // cleanup
	.uleb128 .Ltmp21-.Lfunc_begin2  // abort and the pad's call
	.uleb128 .Lfunc_end2-.Ltmp21
	.byte	0                       // no landing pad
	.byte	0
.Lcst_end2:
	.p2align	2

	.hidden	DW.ref.__gxx_personality_v0
	.weak	DW.ref.__gxx_personality_v0
	.section	.data.DW.ref.__gxx_personality_v0,"awG",@progbits,DW.ref.__gxx_personality_v0,comdat
	.p2align	3, 0x0
	.type	DW.ref.__gxx_personality_v0,@object
	.size	DW.ref.__gxx_personality_v0, 8
DW.ref.__gxx_personality_v0:
	.xword	__gxx_personality_v0
	.section	".note.GNU-stack","",@progbits

// The bound comes from a compare of the masked 32-bit register; the index is
// the mask of the whole 64-bit register, computed again for the table read,
// as in libc++abi's readEncodedPointer.
	.text
	.globl	masked_lane_dispatch
	.p2align	2
	.type	masked_lane_dispatch,@function
masked_lane_dispatch:
	and	w8, w1, #0xf
	cmp	w8, #0xc
	b.hi	.Ldefault
	adrp	x9, .Ltable
	and	x8, x1, #0xf
	add	x9, x9, :lo12:.Ltable
	ldrsw	x8, [x9, x8, lsl #2]
	add	x8, x8, x9
	br	x8
.Lc0:
	mov	w0, #10
	ret
.Lc1:
	mov	w0, #11
	ret
.Lc2:
	mov	w0, #12
	ret
.Lc3:
	mov	w0, #13
	ret
.Lc4:
	mov	w0, #14
	ret
.Lc5:
	mov	w0, #15
	ret
.Lc6:
	mov	w0, #16
	ret
.Lc7:
	mov	w0, #17
	ret
.Lc8:
	mov	w0, #18
	ret
.Lc9:
	mov	w0, #19
	ret
.Lc10:
	mov	w0, #20
	ret
.Lc11:
	mov	w0, #21
	ret
.Lc12:
	mov	w0, #22
	ret
.Ldefault:
	mov	w0, wzr
	ret
	.size	masked_lane_dispatch, .-masked_lane_dispatch

	.section	.rodata
	.p2align	2
.Ltable:
	.word	.Lc0-.Ltable
	.word	.Lc1-.Ltable
	.word	.Lc2-.Ltable
	.word	.Lc3-.Ltable
	.word	.Lc4-.Ltable
	.word	.Lc5-.Ltable
	.word	.Lc6-.Ltable
	.word	.Lc7-.Ltable
	.word	.Lc8-.Ltable
	.word	.Lc9-.Ltable
	.word	.Lc10-.Ltable
	.word	.Lc11-.Ltable
	.word	.Lc12-.Ltable
// The words after the table are no entries: read as entries, they would
// name addresses far outside the code.
	.word	0x40000000
	.word	0x40000000
	.word	0x40000000
	.section	.note.GNU-stack,"",@progbits

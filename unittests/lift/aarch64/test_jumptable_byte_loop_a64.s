// A byte-code loop that dispatches through a 256-entry compact table on each
// opcode next_op returns, masked to a byte. A zero opcode is skipped: the loop
// head branches to a block after the dispatch, which fetches the next opcode,
// masks it and jumps back, so two masks produce the index.
	.text
	.globl	byte_loop
	.p2align	2
	.type	byte_loop,@function
byte_loop:
	stp	x29, x30, [sp, #-32]!
	mov	x29, sp
	stp	x19, x26, [sp, #16]
	mov	x19, x0
.Lfetch:
	mov	x0, x19
	bl	next_op
	cbz	w0, .Lskip
	and	w26, w0, #0xff
.Ldispatch:
	adrp	x1, .Ltable
	add	x1, x1, :lo12:.Ltable
	ldrh	w1, [x1, w26, uxtw #1]
	adr	x2, .Lanchor
	add	x1, x2, w1, sxth #2
	br	x1
.Lanchor:
.Lop0:
	mov	x0, x19
	mov	w1, #0
	bl	apply_op
	b	.Lfetch
.Lop1:
	mov	x0, x19
	mov	w1, #1
	bl	apply_op
	b	.Lfetch
.Lop2:
	mov	x0, x19
	mov	w1, #2
	bl	apply_op
	b	.Lfetch
.Lop3:
	mov	x0, x19
	mov	w1, #3
	bl	apply_op
	b	.Lfetch
.Lop4:
	mov	x0, x19
	mov	w1, #4
	bl	apply_op
	b	.Lfetch
.Lop5:
	mov	x0, x19
	mov	w1, #5
	bl	apply_op
	b	.Lfetch
.Lop6:
	mov	x0, x19
	mov	w1, #6
	bl	apply_op
	b	.Lfetch
.Lop7:
	ldp	x19, x26, [sp, #16]
	ldp	x29, x30, [sp], #32
	ret
.Lskip:
	mov	x0, x19
	bl	next_op
	and	w26, w0, #0xff
	b	.Ldispatch
	.size	byte_loop, .-byte_loop

	.section	.rodata
	.p2align	1
.Ltable:
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
	.hword	(.Lop0-.Lanchor)/4
	.hword	(.Lop1-.Lanchor)/4
	.hword	(.Lop2-.Lanchor)/4
	.hword	(.Lop3-.Lanchor)/4
	.hword	(.Lop4-.Lanchor)/4
	.hword	(.Lop5-.Lanchor)/4
	.hword	(.Lop6-.Lanchor)/4
	.hword	(.Lop7-.Lanchor)/4
// Other data follows the table.
	.p2align	3
.Lmessage:
	.asciz	"byte loop done"
	.section	.note.GNU-stack,"",@progbits

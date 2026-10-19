// A trap ends its block: `trap_or_five` returns 5 or traps, and nothing
// follows the trap.  `brk #1` is clang's __builtin_trap().
	.text
	.globl	trap_or_five
	.type	trap_or_five,%function
trap_or_five:
	cbz	w0, 1f
	mov	w0, #5
	ret
1:
	brk	#1
	.size	trap_or_five, .-trap_or_five

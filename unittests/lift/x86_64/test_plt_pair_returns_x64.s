# Calls through PLT entries whose callees return a register pair.  Linked
# into a shared object, pair_of is preemptible, so sum_pair reaches this
# image's own routine through its PLT entry; __udivti3 is another module's.
#   struct pair { long a, b; };
#   struct pair pair_of(long x) { return (struct pair){x * 3, x + 7}; }
#   long sum_pair(long x) { struct pair p = pair_of(x); return p.a + p.b; }
#   struct pair pair_of7(long a, long b, long c, long d, long e, long f,
#                        long g) {
#     return (struct pair){a + b + c + d + e + f + g, b};
#   }
#   long sum_pair7(long x) {
#     struct pair p = pair_of7(x, 2, 3, 4, 5, 6, x * 7);
#     return p.a + p.b;
#   }
#   unsigned long high_quotient(unsigned __int128 n, unsigned long d) {
#     return (unsigned long)((n / d) >> 64);
#   }

	.text
	.p2align 4
	.globl	pair_of
	.type	pair_of, @function
pair_of:
	leaq	(%rdi,%rdi,2), %rax
	leaq	7(%rdi), %rdx
	ret
	.size	pair_of, .-pair_of

	.p2align 4
	.globl	sum_pair
	.type	sum_pair, @function
sum_pair:
	subq	$8, %rsp
	call	pair_of@PLT
	addq	$8, %rsp
	addq	%rdx, %rax
	ret
	.size	sum_pair, .-sum_pair

	.p2align 4
	.globl	pair_of7
	.type	pair_of7, @function
pair_of7:
	leaq	(%rdi,%rsi), %rax
	addq	%rdx, %rax
	addq	%rcx, %rax
	addq	%r8, %rax
	addq	%r9, %rax
	addq	8(%rsp), %rax
	movq	%rsi, %rdx
	ret
	.size	pair_of7, .-pair_of7

	.p2align 4
	.globl	sum_pair7
	.type	sum_pair7, @function
sum_pair7:
	subq	$24, %rsp
	leaq	(,%rdi,8), %rax
	subq	%rdi, %rax
	movq	%rax, (%rsp)
	movl	$2, %esi
	movl	$3, %edx
	movl	$4, %ecx
	movl	$5, %r8d
	movl	$6, %r9d
	call	pair_of7@PLT
	addq	$24, %rsp
	addq	%rdx, %rax
	ret
	.size	sum_pair7, .-sum_pair7

	.p2align 4
	.globl	high_quotient
	.type	high_quotient, @function
high_quotient:
	subq	$8, %rsp
	xorl	%ecx, %ecx
	call	__udivti3@PLT
	addq	$8, %rsp
	movq	%rdx, %rax
	ret
	.size	high_quotient, .-high_quotient
	.section	.note.GNU-stack,"",@progbits

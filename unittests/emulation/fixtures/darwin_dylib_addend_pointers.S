// Original ordinary bind with a negative signed addend.
.section __DATA,__data
.p2align 3
.globl _from_tail
_from_tail:
.quad _provider_tail - 4

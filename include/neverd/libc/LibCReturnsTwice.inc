// Functions whose call sites require returns-twice register semantics.
// Included by LibCCallTraits.cpp with LIBC_RETURNS_TWICE_SYMBOL defined.

LIBC_RETURNS_TWICE_SYMBOL("setjmp")
LIBC_RETURNS_TWICE_SYMBOL("sigsetjmp")
LIBC_RETURNS_TWICE_SYMBOL("vfork")

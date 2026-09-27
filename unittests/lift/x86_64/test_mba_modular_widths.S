// Mixed Boolean/arithmetic identities with machine-width multiplication.
// The low w bits of y * (2^w - 1) are -y; the full product is different.

#if defined(__APPLE__)
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif

#if defined(_WIN32)
#define ARG_X %rcx
#define ARG_Y %rdx
#define ARG_PTR %r8
#else
#define ARG_X %rdi
#define ARG_Y %rsi
#define ARG_PTR %rdx
#endif

.text

.macro modular_sub name, width, parity=0, store=0
.globl \name
#if defined(__ELF__)
.type \name, @function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  .if \store
    movq ARG_PTR, %r11
  .endif
  movq ARG_X, %r8
  movq ARG_Y, %r9
  // (x & y) | (x & ~y) = x, independently of the product below.
  movq %r8, %r10
  andq %r9, %r10
  movq %r9, %rax
  notq %rax
  andq %rax, %r8
  orq %r10, %r8
  movq %r9, %rax
  movq $-1, %r10
  .if \width == 8
    mulb %r10b
    addb %r8b, %al
  .elseif \width == 16
    mulw %r10w
    addw %r8w, %ax
  .elseif \width == 32
    mull %r10d
    addl %r8d, %eax
  .else
    mulq %r10
    addq %r8, %rax
  .endif
  .if \parity
    setp %al
    movzbl %al, %eax
  .elseif \width == 8
    movzbl %al, %eax
  .elseif \width == 16
    movzwl %ax, %eax
  .endif
  .if \store
    movq %rax, (%r11)
  .endif
  retq
#if defined(__ELF__)
.size \name, .-\name
#endif
.endm

modular_sub SYMBOL(mba_sub8), 8
modular_sub SYMBOL(mba_sub16), 16
modular_sub SYMBOL(mba_sub32), 32
modular_sub SYMBOL(mba_sub64), 64
modular_sub SYMBOL(mba_parity8), 8, 1
modular_sub SYMBOL(mba_parity16), 16, 1
modular_sub SYMBOL(mba_parity32), 32, 1
modular_sub SYMBOL(mba_parity64), 64, 1
modular_sub SYMBOL(mba_store64), 64, 0, 1

// Keep the same inputs in private frame slots across repeated loads. This
// exercises expression recovery after ordinary x64 register spilling.
.macro spilled_mba name, subtract=0
.globl \name
#if defined(__ELF__)
.type \name, @function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  subq $24, %rsp
  movq ARG_X, 0(%rsp)
  movq ARG_Y, 8(%rsp)
  movq 0(%rsp), %r8
  movq 8(%rsp), %r9
  movq 0(%rsp), %r10
  movq 8(%rsp), %r11
  xorq %r9, %r8
  .if \subtract
    notq %r10
  .endif
  andq %r11, %r10
  shlq $1, %r10
  .if \subtract
    subq %r10, %r8
  .else
    addq %r10, %r8
  .endif
  movq %r8, %rax
  addq $24, %rsp
  retq
#if defined(__ELF__)
.size \name, .-\name
#endif
.endm

spilled_mba SYMBOL(mba_spilled_add)
spilled_mba SYMBOL(mba_spilled_sub), 1

// If t = x | y, then (t - x) | t = t. Preserve one shared computation of t
// so the decompiler must reason through both of its uses.
.macro absorbed_or name, width
.globl \name
#if defined(__ELF__)
.type \name, @function
#elif defined(_WIN32)
.def \name; .scl 2; .type 32; .endef
#endif
\name:
  movq ARG_X, %r8
  movq ARG_Y, %rax
  .if \width == 8
    orb %r8b, %al
    movzbl %al, %r9d
    subb %r8b, %al
    orb %r9b, %al
    movzbl %al, %eax
  .elseif \width == 16
    orw %r8w, %ax
    movzwl %ax, %r9d
    subw %r8w, %ax
    orw %r9w, %ax
    movzwl %ax, %eax
  .elseif \width == 32
    orl %r8d, %eax
    movl %eax, %r9d
    subl %r8d, %eax
    orl %r9d, %eax
  .else
    orq %r8, %rax
    movq %rax, %r9
    subq %r8, %rax
    orq %r9, %rax
  .endif
  retq
#if defined(__ELF__)
.size \name, .-\name
#endif
.endm

absorbed_or SYMBOL(mba_or8), 8
absorbed_or SYMBOL(mba_or16), 16
absorbed_or SYMBOL(mba_or32), 32
absorbed_or SYMBOL(mba_or64), 64

// Keeping the upper byte makes multiplication by 255 different from byte
// negation. For example, 2 * 255 is 510, whereas low-byte negation is 254.
.globl SYMBOL(mba_full_product8)
#if defined(__ELF__)
.type SYMBOL(mba_full_product8), @function
#elif defined(_WIN32)
.def SYMBOL(mba_full_product8); .scl 2; .type 32; .endef
#endif
SYMBOL(mba_full_product8):
  movq ARG_X, %rax
  movl $255, %r10d
  mulb %r10b
  movzwl %ax, %eax
  retq
#if defined(__ELF__)
.size SYMBOL(mba_full_product8), .-SYMBOL(mba_full_product8)
.section .note.GNU-stack,"",@progbits
#endif

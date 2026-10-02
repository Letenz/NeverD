.section .text
.global _start
.code16
_start:
  cli
  cld
  lgdtl %cs:(gdt_pointer-_start)
  movl %cr0, %eax
  orl $1, %eax
  movl %eax, %cr0
  .byte 0x66, 0xea
  .long 0xf0000 + (protected-_start)
  .word 8
.code32
protected:
  movw $16, %ax
  movw %ax, %ds
  movw %ax, %es
  movw %ax, %ss
  movw %ax, %fs
  movw %ax, %gs
  movl $0x8000, %esp
  movl $0x1000, %edi
  movl $0x3000/4, %ecx
  xorl %eax, %eax
  rep stosl
  movl $0x2003, 0x1000
  movl $0x3003, 0x2000
  movl $0x83, 0x3000
  movl $0x620, %eax
  movl %eax, %cr4
  movl $0x1000, %eax
  movl %eax, %cr3
  movl $0xc0000080, %ecx
  xorl %edx, %edx
  movl $0x900, %eax
  wrmsr
  movl %cr0, %eax
  andl $0x9fffffff, %eax
  orl $0x80010033, %eax
  movl %eax, %cr0
  .byte 0xea
  .long 0xf0000 + (long_mode-_start)
  .word 24
.code64
long_mode:
  movl $37, %eax
  movw $0x501, %dx
  outl %eax, %dx
  hlt
  jmp .
.balign 8
gdt:
  .quad 0
  .quad 0x00cf9b000000ffff
  .quad 0x00cf93000000ffff
  .quad 0x00af9b000000ffff
gdt_pointer:
  .word 31
  .long 0xf0000+(gdt-_start)
.org 0xfff0
.code16
  ljmp $0xf000, $0
.org 0x10000

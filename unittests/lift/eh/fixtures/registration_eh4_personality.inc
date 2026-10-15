// clang-format off: this include contains x86 assembly.
// Authenticated EH4 wrapper shared by ordinary and nonlocal fixtures.
.p2align 4,0x90
.globl __except_handler4
__except_handler4:
  push ebp
  mov ebp,esp
  push dword ptr [ebp+20]
  push dword ptr [ebp+16]
  push dword ptr [ebp+12]
  push dword ptr [ebp+8]
  .byte 0x68
  .long "@__security_check_cookie@4"
  .byte 0x68
  .long ___security_cookie
  call dword ptr [__imp___except_handler4_common]
  add esp,24
  pop ebp
  ret

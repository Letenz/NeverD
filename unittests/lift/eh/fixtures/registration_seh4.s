// Direct EH4 registration: initialize cookies before publishing FS:[0].
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
  push -2
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
  .long __except_handler4
  mov eax,fs:[0]
  push eax
  sub esp,16
  mov eax,[___security_cookie]
  xor dword ptr [ebp-8],eax
  xor eax,ebp
  mov [ebp-28],eax
#if REGISTRATION_EXPECT_GS
  mov [ebp-32],eax
#endif
  lea eax,[ebp-16]
  .byte 0x64,0x89,0x05
  .long 0
  mov [ebp-24],esp
#define REGISTRATION_SENTINEL -2
#include "registration_seh_body.inc"
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
.safeseh __except_handler4
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#if REGISTRATION_EXPECT_GS
  .long -32,0,-28,0
#else
  .long -2,0,-28,0
#endif
#include "registration_seh_scopes.inc"
.globl __load_config_used
__load_config_used:
  .long 92
  .zero 56
  .long ___security_cookie
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1
#include "registration_cookie_check.inc"

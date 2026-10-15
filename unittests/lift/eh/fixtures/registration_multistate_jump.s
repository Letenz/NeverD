// One physical capture alternates between the outer filter and no active scope.
// The same finally leaves state zero visible for two different unwind bounds.
#if REGISTRATION_MULTISTATE_EH4
#define JUMP_SENTINEL -2
#define JUMP_PERSONALITY __except_handler4
#else
#define JUMP_SENTINEL -1
#define JUMP_PERSONALITY __except_handler3
#endif
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
  push JUMP_SENTINEL
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
  .long JUMP_PERSONALITY
  mov eax,fs:[0]
  push eax
#if REGISTRATION_MULTISTATE_EH4
  sub esp,96
  mov eax,[___security_cookie]
  xor dword ptr [ebp-8],eax
  xor eax,ebp
  mov [ebp-28],eax
#if REGISTRATION_EXPECT_GS
  mov [ebp-32],eax
#endif
  lea eax,[ebp-16]
  mov fs:[0],eax
#else
  mov fs:[0],esp
  sub esp,96
#endif
  mov [ebp-24],esp
  mov [ebp-104],ebx
  mov ebx,6
  mov dword ptr [ebp-100],0
#if REGISTRATION_MULTISTATE_EH4
  .byte 0x68
  .long ___security_cookie
#endif
  test dword ptr [_registration_iteration],1
  jnz .Lcapture_inside
  mov dword ptr [ebp-4],JUMP_SENTINEL
  push JUMP_SENTINEL
  jmp .Lcapture_join
.Lcapture_inside:
  mov dword ptr [ebp-4],0
  push 0
.Lcapture_join:
  .byte 0x68
#if REGISTRATION_MULTISTATE_EH4
  .long __seh_longjmp_unwind4
  push 3
#else
  .long __seh_longjmp_unwind
  push 2
#endif
  lea eax,[ebp-96]
  push eax
  call __setjmp3
#if REGISTRATION_MULTISTATE_EH4
  add esp,20
#else
  add esp,16
#endif
  test eax,eax
  jnz .Lresumed
  mov dword ptr [ebp-4],1
  mov ebx,9
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
.Lresumed:
  test dword ptr [_registration_iteration],1
  jnz .Lexpect_inside
  cmp dword ptr [ebp-4],JUMP_SENTINEL
  jne .Lbad
  jmp .Llevel_checked
.Lexpect_inside:
  cmp dword ptr [ebp-4],0
  jne .Lbad
.Llevel_checked:
  cmp ebx,6
  jne .Lbad
  cmp eax,42
  jne .Lbad
  mov dword ptr [ebp-4],0
  call _registration_raise
.Lbad:
  mov dword ptr [ebp-4],JUMP_SENTINEL
  mov eax,91
  jmp .Lfinish
.Lfilter:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,3
  mov [_registration_trace],eax
  mov eax,1
  ret
.Lfinally:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,2
  mov [ebp-100],eax
  ret
.Lhandler:
  mov eax,7
.Lfinish:
#if REGISTRATION_EXPECT_GS
  mov ecx,[ebp-32]
  xor ecx,ebp
  call "@__security_check_cookie@4"
#endif
  mov ebx,[ebp-104]
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
#if REGISTRATION_MULTISTATE_EH4
#include "registration_eh4_personality.inc"
#endif
.safeseh JUMP_PERSONALITY
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#if REGISTRATION_MULTISTATE_EH4
#if REGISTRATION_EXPECT_GS
  .long -32,0,-28,0
#else
  .long -2,0,-28,0
#endif
#endif
  .long JUMP_SENTINEL,.Lfilter,.Lhandler
  .long 0,0,.Lfinally
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
#if REGISTRATION_MULTISTATE_EH4
  .zero 56
  .long ___security_cookie
#else
  .zero 60
#endif
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1
#if REGISTRATION_MULTISTATE_EH4
#include "registration_cookie_check.inc"
#endif

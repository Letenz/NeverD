// A finally restores the parent's SavedESP and branches out of its invocation.
// Cases include ordinary CALL, runtime unwind, two parent destinations, a
// callback with both near-return and parent-jump exits, and a subsequent throw.
#ifndef REGISTRATION_PARENT_RESTORE_CHAIN
#define REGISTRATION_PARENT_RESTORE_CHAIN 1
#endif
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
#if REGISTRATION_PARENT_EH4
#define REGISTRATION_SENTINEL -2
  push -2
#else
#define REGISTRATION_SENTINEL -1
  push -1
#endif
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
#if REGISTRATION_PARENT_EH4
  .long __except_handler4
#else
  .long __except_handler3
#endif
  mov eax,fs:[0]
  push eax
#if !REGISTRATION_PARENT_EH4
  mov fs:[0],esp
#endif
  sub esp,32
#if REGISTRATION_PARENT_EH4
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
#endif
  mov [ebp-24],esp
  mov [ebp-40],ebx
  mov [ebp-44],esi
  mov [ebp-48],edi
  mov ebx,11
  mov esi,22
  mov edi,33
  mov dword ptr [ebp-36],0
#if REGISTRATION_CASE == 88
  // This input-register case invokes the finally only through an ordinary
  // CALL. CRT dispatch does not promise the incoming data GPRs of its caller.
  mov dword ptr [ebp-4],0
#else
  mov dword ptr [ebp-4],1
#endif
#if REGISTRATION_CASE == 83 || REGISTRATION_CASE == 86 || REGISTRATION_CASE == 90
  call _registration_raise
#else
  call _registration_no_raise
  mov dword ptr [ebp-4],0
  call .Lfinally
#endif
#if REGISTRATION_CASE == 84
  test dword ptr [_registration_iteration],1
  jnz .Lparent_alternative
#endif
#if REGISTRATION_CASE == 89 || REGISTRATION_CASE == 90
  // The resumed parent tail has no ordinary predecessor. Its checked paths
  // still end at the shared parent epilogue.
  jmp .Lfinish
#else
  jmp .Lparent_resume
#endif
.Lfinally:
  mov dword ptr [ebp-36],2
#if REGISTRATION_CASE == 88
  // The ordinary invocation must also carry its caller's current values into
  // the callback; replacing only the outputs would pass constant assignments.
  sub ebx,6
  sub esi,16
  sub edi,26
#else
  mov ebx,5
  mov esi,6
  mov edi,7
#endif
#if REGISTRATION_CASE == 85 || REGISTRATION_CASE == 88
  test dword ptr [_registration_iteration],1
  jnz .Lparent_exit
  ret
.Lparent_exit:
#endif
  mov esp,[ebp-24]
#if REGISTRATION_CASE == 84
  test dword ptr [_registration_iteration],1
  jnz .Lalternative_exit
#endif
  jmp .Lparent_resume
#if REGISTRATION_CASE == 84
.Lalternative_exit:
  jmp .Lparent_alternative
.Lparent_alternative:
  mov ecx,[ebp-36]
  imul ecx,ecx,10
  add ecx,4
  mov [_registration_trace],ecx
  mov eax,7
  jmp .Lfinish
#endif
.Lparent_resume:
#if (REGISTRATION_CASE == 86 || REGISTRATION_CASE == 87) && REGISTRATION_PARENT_RESTORE_CHAIN
  // Abnormal transfer bypassed the CRT's temporary unwind guard. Reconnect
  // the live source registration before this parent can dispatch again.
  // This also covers asynchronous entry from the ordinary-call test path.
  lea ecx,[ebp-16]
  mov fs:[0],ecx
#endif
  mov ecx,[ebp-36]
  imul ecx,ecx,10
  add ecx,3
  mov [_registration_trace],ecx
#if REGISTRATION_CASE == 86 || REGISTRATION_CASE == 87
  // A later except-body entry has its own CRT register context. Observe the
  // finally's current values at the resumed parent before dispatching again.
  cmp ebx,5
  jne .Lbad_registers
  cmp esi,6
  jne .Lbad_registers
  cmp edi,7
  jne .Lbad_registers
  mov dword ptr [ebp-4],0
  call _registration_raise
  mov eax,91
#else
  mov eax,7
#endif
  jmp .Lfinish
.Lfilter:
  mov eax,1
  ret
.Lhandler:
  mov esp,[ebp-24]
#if REGISTRATION_CASE == 86 || REGISTRATION_CASE == 87
  mov ecx,[_registration_trace]
  imul ecx,ecx,10
  add ecx,4
  mov [_registration_trace],ecx
  mov eax,7
#else
  mov eax,92
#endif
.Lfinish:
  // A native context restores the machine frame, while the source registers
  // retain the values written by this finally at its parent continuation.
#if REGISTRATION_CASE == 86 || REGISTRATION_CASE == 87
  jmp .Lrestore_registers
#else
  cmp ebx,5
  jne .Lbad_registers
  cmp esi,6
  jne .Lbad_registers
  cmp edi,7
  je .Lrestore_registers
#endif
.Lbad_registers:
  mov eax,93
.Lrestore_registers:
  mov ebx,[ebp-40]
  mov esi,[ebp-44]
  mov edi,[ebp-48]
#if REGISTRATION_EXPECT_GS
  mov ecx,[ebp-32]
  xor ecx,ebp
  call "@__security_check_cookie@4"
#endif
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
#if REGISTRATION_PARENT_EH4
#include "registration_eh4_personality.inc"
.safeseh __except_handler4
#else
.safeseh __except_handler3
#endif
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#if REGISTRATION_PARENT_EH4
#if REGISTRATION_EXPECT_GS
  .long -32,0,-28,0
#else
  .long -2,0,-28,0
#endif
#endif
  .long REGISTRATION_SENTINEL,.Lfilter,.Lhandler
  .long 0,0,.Lfinally
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
  .zero 56
#if REGISTRATION_PARENT_EH4
  .long ___security_cookie
#else
  .long 0
#endif
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1
#if REGISTRATION_PARENT_EH4
#include "registration_cookie_check.inc"
#endif

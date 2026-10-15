// Callback-selected states must update the physical parent registration.
// Cases 30/31 activate a scope used only by a callback before a nonlocal jump.
// Cases 32/33 retire a pending cleanup while the CRT is already unwinding.
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
  push -1
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
  .long __except_handler3
  mov eax,fs:[0]
  push eax
  mov fs:[0],esp
  sub esp,96
  mov [ebp-24],esp
  mov [ebp-104],ebx
  mov ebx,6
  mov dword ptr [ebp-100],0
  mov dword ptr [ebp-4],0
  push 0
  .byte 0x68
  .long __seh_longjmp_unwind
  push 2
  lea eax,[ebp-96]
  push eax
  call __setjmp3
  add esp,16
  test eax,eax
  jnz .Lresumed
  mov ebx,9
#if REGISTRATION_CASE == 30 || REGISTRATION_CASE == 34 || REGISTRATION_CASE == 35
  // State 1 has a source parent boundary; state 2 has only the callback write.
  mov dword ptr [ebp-4],1
  mov dword ptr [ebp-4],0
  call .Lfinally
  jmp .Lbad
#else
#if REGISTRATION_CASE == 31
  mov dword ptr [ebp-4],1
#else
  mov dword ptr [ebp-4],2
#endif
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#endif
.Lresumed:
#if REGISTRATION_CASE == 33
  cmp dword ptr [ebp-4],-1
#else
  cmp dword ptr [ebp-4],0
#endif
  jne .Lbad
  cmp ebx,6
  jne .Lbad
  cmp eax,42
  jne .Lbad
#if REGISTRATION_CASE == 33
  mov dword ptr [ebp-4],0
#endif
  call _registration_raise
.Lbad:
  mov dword ptr [ebp-4],-1
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
#if REGISTRATION_CASE == 30 || REGISTRATION_CASE == 31 || REGISTRATION_CASE == 34 || REGISTRATION_CASE == 35
#if REGISTRATION_CASE == 34
  mov byte ptr [ebp-4],2
#elif REGISTRATION_CASE == 35
  mov word ptr [ebp-4],2
#else
  mov dword ptr [ebp-4],2
#endif
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#elif REGISTRATION_CASE == 32
  mov dword ptr [ebp-4],0
  ret
#else
  mov dword ptr [ebp-4],-1
  ret
#endif
.Linner_finally:
  mov eax,[ebp-100]
  imul eax,eax,10
#if REGISTRATION_CASE == 30 || REGISTRATION_CASE == 31 || REGISTRATION_CASE == 34 || REGISTRATION_CASE == 35
  add eax,4
#else
  add eax,9
#endif
  mov [ebp-100],eax
  ret
.Lhandler:
  mov eax,7
.Lfinish:
  mov ebx,[ebp-104]
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
.Lscope_table:
  .long -1,.Lfilter,.Lhandler
#if REGISTRATION_CASE == 30 || REGISTRATION_CASE == 31 || REGISTRATION_CASE == 34 || REGISTRATION_CASE == 35
  .long 0,0,.Lfinally
  .long 0,0,.Linner_finally
#else
  .long 0,0,.Linner_finally
  .long 1,0,.Lfinally
#endif
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
  .zero 60
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1

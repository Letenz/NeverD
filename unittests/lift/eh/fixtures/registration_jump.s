// Native EH3 capture, nonlocal finally unwind, then another exception.
#if REGISTRATION_CASE == 16 || REGISTRATION_CASE == 17
#define FILTER_JUMP 1
#endif
#if REGISTRATION_CASE == 18 || REGISTRATION_CASE == 19
#define RETIRED_JUMP 1
#endif
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
#if RETIRED_JUMP
  mov dword ptr [ebp-108],0
#endif
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-4],1
  push 1
#elif FILTER_JUMP
  mov dword ptr [ebp-4],-1
  push -1
#else
  mov dword ptr [ebp-4],0
  push 0
#endif
  .byte 0x68
  .long __seh_longjmp_unwind
  push 2
  lea eax,[ebp-96]
  push eax
  call __setjmp3
  add esp,16
  test eax,eax
  jnz .Lresumed
#if RETIRED_JUMP
  mov dword ptr [ebp-108],1
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-4],0
#else
  mov dword ptr [ebp-4],-1
#endif
  mov ebx,9
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-100],2
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#else
  call .Lfinally
  jmp .Lbad
#endif
#elif FILTER_JUMP
  mov dword ptr [ebp-4],0
  mov ebx,9
.Lfault:
  mov eax,dword ptr [0]
  jmp .Lfault
#elif REGISTRATION_CASE == 20 || REGISTRATION_CASE == 21 || REGISTRATION_CASE == 22
  mov dword ptr [ebp-4],0
  mov dword ptr [ebp-100],0
  mov ebx,9
  sub esp,160
#if REGISTRATION_CASE == 22
  push 42
#endif
  lea eax,[ebp-100]
  push eax
  lea eax,[ebp-96]
  push eax
  call _registration_jump_callee
#if REGISTRATION_CASE == 22
  add esp,172
#else
  add esp,168
#endif
#if REGISTRATION_CASE == 21
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#endif
  jmp .Lbad
#else
  mov dword ptr [ebp-4],1
  mov ebx,9
#if REGISTRATION_CASE == 13 || REGISTRATION_CASE == 15
  mov dword ptr [ebp-4],0
  call .Lfinally
  jmp .Lbad
#else
#if REGISTRATION_CASE == 10 || REGISTRATION_CASE == 15
  push 0
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#endif
#endif
.Lresumed:
#if REGISTRATION_CASE == 12
  cmp dword ptr [ebp+8],7
  jne .Lbad
#endif
#if FILTER_JUMP || REGISTRATION_CASE == 18
  cmp dword ptr [ebp-4],-1
#else
  cmp dword ptr [ebp-4],0
#endif
  jne .Lbad
  cmp ebx,6
  jne .Lbad
#if REGISTRATION_CASE == 10 || REGISTRATION_CASE == 15 || REGISTRATION_CASE == 17
  cmp eax,1
#else
  cmp eax,42
#endif
  jne .Lbad
#if FILTER_JUMP || REGISTRATION_CASE == 18
#if RETIRED_JUMP
  mov dword ptr [ebp-4],-1
#endif
  call _registration_no_raise
  mov eax,7
  jmp .Lfinish
#else
#if REGISTRATION_CASE == 19
  mov dword ptr [ebp-4],0
#endif
  call _registration_raise
#endif
.Lbad:
  mov dword ptr [ebp-4],-1
  mov eax,91
  jmp .Lfinish
.Lfilter:
#if FILTER_JUMP
  mov dword ptr [_registration_trace],23
#if REGISTRATION_CASE == 17
  push 0
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#else
  mov eax,[ebp-100]
  imul eax,eax,10
#if REGISTRATION_CASE == 12
  add eax,[ebp+8]
  sub eax,4
#else
  add eax,3
#endif
  mov [_registration_trace],eax
  mov eax,1
  ret
#endif
.Lfinally:
#if RETIRED_JUMP
  cmp dword ptr [ebp-108],0
  je .Lfinally_return
  mov dword ptr [_registration_trace],23
#endif
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,2
  mov [ebp-100],eax
#if REGISTRATION_CASE >= 13
#if REGISTRATION_CASE == 15
  push 0
#else
  push 42
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#else
  ret
#endif
#if RETIRED_JUMP
.Lfinally_return:
  ret
#endif
#if REGISTRATION_CASE == 19
.Linner_handler:
  mov dword ptr [ebp-4],-1
  mov eax,94
  jmp .Lfinish
#endif
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
#if REGISTRATION_CASE == 18
  .long -1,0,.Lfinally
#else
  .long -1,.Lfilter,.Lhandler
#if !FILTER_JUMP && REGISTRATION_CASE != 20 && REGISTRATION_CASE != 21 && REGISTRATION_CASE != 22
#if REGISTRATION_CASE == 19
  .long 0,.Lfilter,.Linner_handler
#elif REGISTRATION_CASE == 11
  .long 0,.Lfilter,.Lbad
#else
  .long 0,0,.Lfinally
#endif
#endif
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

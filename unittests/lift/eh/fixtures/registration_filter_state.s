// A finally raises during nonlocal unwind. Its inner filter shares a return
// between continue-execution and continue-search paths with correlated states.
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
#if REGISTRATION_CASE == 41
  // Install every scope before capture so this case isolates the two distinct
  // unwind projections from reconstruction of unreachable table entries.
  mov dword ptr [ebp-4],2
  mov dword ptr [ebp-4],1
  push 1
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
  mov ebx,9
#if REGISTRATION_CASE == 41
  mov dword ptr [ebp-4],4
#else
  mov dword ptr [ebp-4],2
#endif
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
.Lresumed:
  cmp ebx,6
  jne .Lbad
  cmp eax,42
  jne .Lbad
#if REGISTRATION_CASE == 37
  cmp dword ptr [ebp-4],0
#elif REGISTRATION_CASE == 38
  cmp dword ptr [ebp-4],-1
#elif REGISTRATION_CASE == 41
  mov ecx,[_registration_iteration]
  and ecx,1
  xor ecx,1
  cmp [ebp-4],ecx
#else
  mov ecx,[_registration_iteration]
  and ecx,1
  sub ecx,1
  cmp [ebp-4],ecx
#endif
  jne .Lbad
  mov eax,[ebp-100]
  mov [_registration_trace],eax
  mov dword ptr [ebp-4],-1
  call _registration_no_raise
  mov eax,7
  jmp .Lfinish
.Lbad:
  mov dword ptr [ebp-4],-1
  mov eax,91
  jmp .Lfinish
.Lfilter:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,3
  mov [ebp-100],eax
  mov eax,[_registration_iteration]
  test eax,1
  jz .Ldirect_continue
#if REGISTRATION_CASE == 38
  mov dword ptr [ebp-4],-1
#elif REGISTRATION_CASE == 40
  mov dword ptr [ebp-4],1
#elif REGISTRATION_CASE == 41
  mov dword ptr [ebp-4],3
#else
  mov dword ptr [ebp-4],0
#endif
  mov eax,0
  jmp .Lfilter_return
.Ldirect_continue:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,4
  mov [ebp-100],eax
#if REGISTRATION_CASE == 37
  mov dword ptr [ebp-4],0
#elif REGISTRATION_CASE == 41
  mov dword ptr [ebp-4],2
#else
  mov dword ptr [ebp-4],-1
#endif
#if REGISTRATION_CASE == 39
  mov eax,-2
#else
  mov eax,-1
#endif
.Lfilter_return:
  ret
.Louter_filter:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,4
  mov [ebp-100],eax
#if REGISTRATION_CASE == 39
  mov eax,0x80000000
#else
  mov eax,-1
#endif
  ret
.Lfinally:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,2
  mov [ebp-100],eax
  push 0
  push 0
  push 0
  push 0xe0420042
  call dword ptr [__imp__RaiseException@16]
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,5
  mov [ebp-100],eax
  ret
.Louter_handler:
  mov dword ptr [ebp-4],-1
  mov eax,96
  jmp .Lfinish
#if REGISTRATION_CASE == 41
.Lmiddle_filter:
  mov eax,-1
  ret
.Lskipped_filter:
  mov eax,-1
  ret
.Lmiddle_handler:
  mov dword ptr [ebp-4],-1
  mov eax,94
  jmp .Lfinish
.Lskipped_handler:
  mov dword ptr [ebp-4],-1
  mov eax,95
  jmp .Lfinish
#endif
.Lhandler:
  mov dword ptr [ebp-4],-1
  mov eax,97
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
  .long -1,.Louter_filter,.Louter_handler
#if REGISTRATION_CASE == 41
  .long 0,.Lmiddle_filter,.Lmiddle_handler
  .long 1,.Lskipped_filter,.Lskipped_handler
  .long 0,.Lfilter,.Lhandler
  .long 3,0,.Lfinally
#else
  .long 0,.Lfilter,.Lhandler
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

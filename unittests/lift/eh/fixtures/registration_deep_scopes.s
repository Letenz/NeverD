// A 259-row nested scope chain traverses 254 search-only scopes during
// nonlocal unwind. Alternating invocations still observe one or two finally
// calls, exact resumed registers/state and the restored registration chain.
// Distinct source call sites keep this depth test independent of call cloning.
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
  mov eax,[_registration_iteration]
  test eax,1
  jnz .Lhigh
  mov dword ptr [ebp-4],2
  push 42
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
.Lhigh:
  mov dword ptr [ebp-4],258
.Ljump:
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
  cmp dword ptr [ebp-4],0
  jne .Lbad
  mov eax,[_registration_iteration]
  and eax,1
  add eax,1
  cmp [ebp-100],eax
  jne .Lbad
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
  mov byte ptr [ebp-4],0
  mov eax,-1
  ret
.Louter_filter:
  mov eax,-1
  ret
.Lfinally:
  mov eax,[ebp-100]
  add eax,1
  mov [ebp-100],eax
  push 0
  push 0
  push 0
  push 0xe0420042
  call dword ptr [__imp__RaiseException@16]
  ret
.Louter_handler:
  mov dword ptr [ebp-4],-1
  mov eax,96
  jmp .Lfinish
.Llow_handler:
  mov dword ptr [ebp-4],-1
  mov eax,97
  jmp .Lfinish
.Lhigh_handler:
  mov dword ptr [ebp-4],-1
  mov eax,98
  jmp .Lfinish
.altmacro
.macro skip_callback index
.Lfilter\index:
  mov eax,0
  ret
.Lhandler\index:
  mov dword ptr [ebp-4],-1
  mov eax,99
  jmp .Lfinish
.endm
.set .Lindex,3
.rept 254
skip_callback %.Lindex
.set .Lindex,.Lindex+1
.endr
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
  .long 0,.Lfilter,.Llow_handler
  .long 1,0,.Lfinally
.macro skip_scope index
  .long \index-1,.Lfilter\index,.Lhandler\index
.endm
.set .Lindex,3
.rept 254
skip_scope %.Lindex
.set .Lindex,.Lindex+1
.endr
  .long 256,.Lfilter,.Lhigh_handler
  .long 257,0,.Lfinally
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

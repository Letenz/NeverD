// Canonical MSVC SEH3 frame with an exported, symbol-free ownership boundary.
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
  sub esp,16
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 2 || REGISTRATION_CASE == 4
  mov dword ptr [ebp-4],1
#else
  mov dword ptr [ebp-4],0
#endif
#if REGISTRATION_CASE == 4
  call _registration_no_raise
#else
  call _registration_raise
#endif
  mov dword ptr [ebp-4],-1
#if REGISTRATION_CASE == 4
  call .Lfinally
#endif
#if REGISTRATION_CASE == 3 || REGISTRATION_CASE == 4
  mov eax,7
#else
  mov eax,9
#endif
  jmp .Lfinish
.Lfilter:
#if REGISTRATION_CASE == 1
  mov eax,[ebp-4]
  mov [_registration_trace],eax
  mov eax,1
#elif REGISTRATION_CASE == 2
  mov eax,[_registration_trace]
  imul eax,eax,10
  add eax,2
  mov [_registration_trace],eax
  mov eax,1
#elif REGISTRATION_CASE == 5 || REGISTRATION_CASE == 6
  mov eax,[ebp+8]
  mov [_registration_trace],eax
  mov eax,1
#elif REGISTRATION_CASE == 3
  mov eax,[ebp-4]
  add eax,1
  mov [_registration_trace],eax
  mov eax,-1
#else
  mov eax,[ebp-4]
  add eax,1
#endif
  ret
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 4
.Lfinally:
#if REGISTRATION_CASE == 4
  mov eax,[ebp-4]
  add eax,3
#else
  mov eax,[_registration_trace]
  imul eax,eax,10
  add eax,2
#endif
  mov [_registration_trace],eax
  ret
#elif REGISTRATION_CASE == 2
.Linnerfilter:
  mov eax,[ebp-4]
  mov [_registration_trace],eax
  xor eax,eax
  ret
.Linnerhandler:
  mov dword ptr [ebp-4],-1
  mov eax,91
  jmp .Lfinish
#endif
.Lhandler:
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 2
  mov eax,[_registration_trace]
  imul eax,eax,10
  add eax,3
  mov [_registration_trace],eax
#endif
#if REGISTRATION_CASE == 3 || REGISTRATION_CASE == 4
  mov eax,97
#elif REGISTRATION_CASE == 5
  mov eax,[ebp+8]
#elif REGISTRATION_CASE == 6
  add dword ptr [ebp+8],10
  mov eax,[ebp+8]
  sub eax,10
#else
  mov eax,7
#endif
.Lfinish:
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
#if REGISTRATION_CASE == 6
.globl _registration_call_entry
_registration_call_entry:
  push 7
  call _registration_entry
  mov edx,[esp]
  mov [_registration_parameter_after],edx
  add esp,4
  ret
#endif
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
.Lscope_table:
  .long -1,.Lfilter,.Lhandler
#if REGISTRATION_CASE == 1 || REGISTRATION_CASE == 4
  .long 0,0,.Lfinally
#elif REGISTRATION_CASE == 2
  .long 0,.Linnerfilter,.Linnerhandler
#endif
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
  .zero 60
  .long ___safe_se_handler_table
  .long ___safe_se_handler_count
  // An advertised but initially empty Guard CF table has no pointer fixup.
  // Installing its first generated entry must add a new HIGHLOW relocation.
  .long 0,0,0,0,0x400
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1

// One throwing source call is reached with either independent SEH state.
// The helper records its actual caller before RaiseException selects the
// matching handler. Alternating iterations must produce traces 1 and 2.
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
  mov [ebp-24],esp
  mov eax,[_registration_iteration]
  test eax,1
  jnz .Lsecond
  mov dword ptr [ebp-4],0
  jmp .Lcall
.Lsecond:
  mov dword ptr [ebp-4],1
.Lcall:
  call _registration_raise
  mov eax,91
  jmp .Lfinish
.Lfilter:
  mov eax,1
  ret
.Lfirst_handler:
  mov dword ptr [ebp-4],-1
  mov dword ptr [_registration_trace],1
  mov eax,7
  jmp .Lfinish
.Lsecond_handler:
  mov dword ptr [ebp-4],-1
  mov dword ptr [_registration_trace],2
  mov eax,7
  jmp .Lfinish
.Lfinish:
  mov ecx,[ebp-16]
  mov fs:[0],ecx
  mov esp,ebp
  pop ebp
  ret
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
.Lscope_table:
  .long -1,.Lfilter,.Lfirst_handler
  .long -1,.Lfilter,.Lsecond_handler
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

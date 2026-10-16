// A finally raises during nonlocal unwind. Its inner filter shares a return
// between continue-execution and continue-search paths with correlated states.
#if REGISTRATION_EH4_FILTER
#define FILTER_SENTINEL -2
#define FILTER_PERSONALITY __except_handler4
#define FILTER_UNWIND __seh_longjmp_unwind4
#else
#define FILTER_SENTINEL -1
#define FILTER_PERSONALITY __except_handler3
#define FILTER_UNWIND __seh_longjmp_unwind
#endif
#define FILTER_REPEATED_JUMP (REGISTRATION_CASE >= 58 && REGISTRATION_CASE <= 67)
#define FILTER_NESTED_JUMP (FILTER_REPEATED_JUMP && (REGISTRATION_CASE & 1))
#define FILTER_OTHER_CAPTURE \
  (REGISTRATION_CASE == 66 || REGISTRATION_CASE == 67 || \
   REGISTRATION_CASE == 70 || REGISTRATION_CASE == 71 || \
   REGISTRATION_CASE == 74 || REGISTRATION_CASE == 75)
#define FILTER_SEARCH_JUMP (REGISTRATION_CASE >= 68 && REGISTRATION_CASE <= 75)
#define FILTER_NESTED_SEARCH (FILTER_SEARCH_JUMP && (REGISTRATION_CASE & 1))
#define FILTER_MIXED_SEARCH (REGISTRATION_CASE >= 72 && REGISTRATION_CASE <= 75)
#define FILTER_SELECTED_SEARCH (REGISTRATION_CASE == 74 || REGISTRATION_CASE == 75)
#define FILTER_DYNAMIC_VALUE (REGISTRATION_CASE == 64 || REGISTRATION_CASE == 65)
#if REGISTRATION_CASE == 60 || REGISTRATION_CASE == 61 || FILTER_OTHER_CAPTURE || FILTER_SEARCH_JUMP
#define FILTER_JUMP_VALUE 43
#define FILTER_RESUME_VALUE 43
#elif REGISTRATION_CASE == 62 || REGISTRATION_CASE == 63
#define FILTER_JUMP_VALUE 0
#define FILTER_RESUME_VALUE 1
#else
#define FILTER_JUMP_VALUE 42
#define FILTER_RESUME_VALUE 42
#endif
.intel_syntax noprefix
.text
.globl _registration_entry
_registration_entry:
  push ebp
  mov ebp,esp
  push FILTER_SENTINEL
  .byte 0x68
  .long .Lscope_table
  .byte 0x68
  .long FILTER_PERSONALITY
  mov eax,fs:[0]
  push eax
#if !REGISTRATION_EH4_FILTER
  mov fs:[0],esp
#endif
#if FILTER_OTHER_CAPTURE
  sub esp,176
#else
  sub esp,96
#endif
#if REGISTRATION_EH4_FILTER
  mov eax,[___security_cookie]
  xor dword ptr [ebp-8],eax
  xor eax,ebp
  mov [ebp-28],eax
#if REGISTRATION_EXPECT_GS
  mov [ebp-32],eax
#endif
  lea eax,[ebp-16]
  mov fs:[0],eax
#endif
  mov [ebp-24],esp
  mov [ebp-104],ebx
  mov ebx,6
  mov dword ptr [ebp-100],0
#if REGISTRATION_CASE == 56
  mov dword ptr [ebp-4],3
#endif
#if REGISTRATION_EH4_FILTER
  .byte 0x68
  .long ___security_cookie
#endif
#if REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
#if REGISTRATION_CASE == 41
  // Case 41 isolates state projections; case 42 first activates scope 2 in
  // the filter, so its callback and handler need independent entry proofs.
  mov dword ptr [ebp-4],2
#endif
  mov dword ptr [ebp-4],1
  push 1
#else
  mov dword ptr [ebp-4],0
  push 0
#endif
  .byte 0x68
  .long FILTER_UNWIND
#if REGISTRATION_EH4_FILTER
  push 3
#else
  push 2
#endif
  lea eax,[ebp-96]
  push eax
  call __setjmp3
#if REGISTRATION_EH4_FILTER
  add esp,20
#else
  add esp,16
#endif
  test eax,eax
  jnz .Lresumed
#if FILTER_OTHER_CAPTURE
  // The second capture owns another stack, saved EBX and unwind bound.
  // Its cleanup abandons it and restores the first capture instead.
  sub esp,16
  mov ebx,8
  mov dword ptr [ebp-4],1
#if REGISTRATION_EH4_FILTER
  .byte 0x68
  .long ___security_cookie
#endif
  push 1
  .byte 0x68
  .long FILTER_UNWIND
#if REGISTRATION_EH4_FILTER
  push 3
#else
  push 2
#endif
  lea eax,[ebp-176]
  push eax
  call __setjmp3
#if REGISTRATION_EH4_FILTER
  add esp,20
#else
  add esp,16
#endif
  test eax,eax
  jnz .Lbad
#endif
  mov ebx,9
#if FILTER_NESTED_SEARCH
  mov dword ptr [ebp-4],3
#elif FILTER_NESTED_JUMP
  mov dword ptr [ebp-4],4
#endif
#if REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
  mov dword ptr [ebp-4],4
#else
  mov dword ptr [ebp-4],2
#endif
  push 42
#if FILTER_OTHER_CAPTURE
  lea eax,[ebp-176]
#else
  lea eax,[ebp-96]
#endif
  push eax
  call __longjmpex
  ud2
.Lresumed:
#if REGISTRATION_CASE >= 53 && REGISTRATION_CASE <= 56
  jmp .Lbad
#else
  cmp ebx,6
  jne .Lbad
#if FILTER_MIXED_SEARCH
  mov ecx,[_registration_iteration]
  and ecx,1
  add ecx,42
  cmp eax,ecx
#elif FILTER_DYNAMIC_VALUE
  mov ecx,[_registration_iteration]
  sub ecx,1
  jnz .Lexpected_value
  mov ecx,1
.Lexpected_value:
  cmp eax,ecx
#else
  cmp eax,FILTER_RESUME_VALUE
#endif
  jne .Lbad
#if REGISTRATION_CASE == 37 || REGISTRATION_CASE == 57 || FILTER_REPEATED_JUMP || FILTER_SEARCH_JUMP
  cmp dword ptr [ebp-4],0
#elif REGISTRATION_CASE == 38
  cmp dword ptr [ebp-4],FILTER_SENTINEL
#elif REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
  mov ecx,[_registration_iteration]
  and ecx,1
  xor ecx,1
  cmp [ebp-4],ecx
#else
  mov ecx,[_registration_iteration]
  and ecx,1
#if REGISTRATION_EH4_FILTER
  shl ecx,1
  sub ecx,2
#else
  sub ecx,1
#endif
  cmp [ebp-4],ecx
#endif
  jne .Lbad
  mov eax,[ebp-100]
  mov [_registration_trace],eax
  mov dword ptr [ebp-4],FILTER_SENTINEL
  call _registration_no_raise
  mov eax,7
  jmp .Lfinish
#endif
.Lbad:
  mov dword ptr [ebp-4],FILTER_SENTINEL
  mov eax,91
  jmp .Lfinish
.Lfilter:
#if REGISTRATION_HAS_CALLBACK_LOOP && REGISTRATION_LOOP_IN_FILTER
  registration_bounded_loop
#endif
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,3
  mov [ebp-100],eax
#if FILTER_SEARCH_JUMP
  mov dword ptr [ebp-4],0
#if FILTER_MIXED_SEARCH
  mov eax,[_registration_iteration]
  test eax,1
  jz .Lsearch_continue
#endif
  push 43
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#if FILTER_MIXED_SEARCH
.Lsearch_continue:
#if FILTER_SELECTED_SEARCH
  mov eax,1
#else
  mov eax,-1
#endif
#endif
#elif REGISTRATION_CASE == 57
  mov eax,[_registration_iteration]
  test eax,1
  jz .Lmixed_continue
  mov eax,1
  jmp .Lmixed_return
.Lmixed_continue:
  mov eax,-1
.Lmixed_return:
#elif REGISTRATION_CASE == 53 || REGISTRATION_CASE == 54 || REGISTRATION_CASE == 56 || FILTER_REPEATED_JUMP
#if REGISTRATION_CASE == 54 || FILTER_REPEATED_JUMP
  // The search cursor stays at scope 1 while its state write adds cleanup 3.
  mov dword ptr [ebp-4],3
#endif
  mov eax,1
#elif REGISTRATION_CASE == 55
  mov dword ptr [ebp-4],FILTER_SENTINEL
  mov eax,0
#else
  mov eax,[_registration_iteration]
  test eax,1
  jz .Ldirect_continue
#if REGISTRATION_CASE == 38
  mov dword ptr [ebp-4],FILTER_SENTINEL
#elif REGISTRATION_CASE == 40
  mov dword ptr [ebp-4],1
#elif REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
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
#elif REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
  mov dword ptr [ebp-4],2
#else
  mov dword ptr [ebp-4],FILTER_SENTINEL
#endif
#if REGISTRATION_CASE == 39
  mov eax,-2
#else
  mov eax,-1
#endif
.Lfilter_return:
#endif
  ret
.Louter_filter:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,4
  mov [ebp-100],eax
#if REGISTRATION_CASE == 55
  mov eax,2
#elif REGISTRATION_CASE == 39
  mov eax,0x80000000
#else
  mov eax,-1
#endif
  ret
.Lfinally:
#if REGISTRATION_HAS_CALLBACK_LOOP && !REGISTRATION_LOOP_IN_FILTER
  registration_bounded_loop
#endif
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,2
  mov [ebp-100],eax
#if REGISTRATION_CASE == 56 || FILTER_NESTED_SEARCH
  call .Lselected_nested
  jmp .Lafter_nested
.Lselected_nested:
#if FILTER_NESTED_SEARCH
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,6
  mov [ebp-100],eax
#endif
#endif
  push 0
  push 0
  push 0
  push 0xe0420042
  call dword ptr [__imp__RaiseException@16]
#if REGISTRATION_CASE == 56 || FILTER_NESTED_SEARCH
#if FILTER_NESTED_SEARCH
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,7
  mov [ebp-100],eax
#endif
  mov eax,0
  ret
.Lafter_nested:
#endif
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,5
  mov [ebp-100],eax
  ret
.Louter_handler:
#if REGISTRATION_CASE == 55
  mov esp,[ebp-24]
  cmp dword ptr [ebp-4],FILTER_SENTINEL
  jne .Lbad
  jmp .Lselected
#endif
  mov dword ptr [ebp-4],FILTER_SENTINEL
  mov eax,96
  jmp .Lfinish
#if REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
.Lmiddle_filter:
  mov eax,-1
  ret
.Lskipped_filter:
  mov eax,-1
  ret
.Lmiddle_handler:
  mov dword ptr [ebp-4],FILTER_SENTINEL
  mov eax,94
  jmp .Lfinish
.Lskipped_handler:
  mov dword ptr [ebp-4],FILTER_SENTINEL
  mov eax,95
  jmp .Lfinish
#endif
.Lhandler:
#if REGISTRATION_CASE == 53 || REGISTRATION_CASE == 54 || REGISTRATION_CASE == 56 || REGISTRATION_CASE == 57 || FILTER_SELECTED_SEARCH
  mov esp,[ebp-24]
  cmp dword ptr [ebp-4],0
  jne .Lbad
  jmp .Lselected
#endif
  mov dword ptr [ebp-4],FILTER_SENTINEL
  mov eax,97
#if (REGISTRATION_CASE >= 53 && REGISTRATION_CASE <= 57) || FILTER_SELECTED_SEARCH
  jmp .Lfinish
.Lselected:
  mov eax,[ebp-100]
  mov [_registration_trace],eax
  mov dword ptr [ebp-4],FILTER_SENTINEL
  call _registration_no_raise
  mov eax,7
#endif
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
#if REGISTRATION_EH4_FILTER
#include "registration_eh4_personality.inc"
#endif
#if REGISTRATION_CASE == 54 || FILTER_REPEATED_JUMP
.Lselected_cleanup:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,6
  mov [ebp-100],eax
#if FILTER_NESTED_JUMP
  call .Lcleanup_jump
  jmp .Lcleanup_return
.Lcleanup_jump:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,7
  mov [ebp-100],eax
#endif
#if FILTER_REPEATED_JUMP
#if FILTER_DYNAMIC_VALUE
  mov eax,[_registration_iteration]
  sub eax,1
  push eax
#else
  push FILTER_JUMP_VALUE
#endif
  lea eax,[ebp-96]
  push eax
  call __longjmpex
  ud2
#endif
#if FILTER_NESTED_JUMP
.Lcleanup_return:
#endif
  ret
#endif
.safeseh FILTER_PERSONALITY
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#if REGISTRATION_EH4_FILTER
#if REGISTRATION_EXPECT_GS
  .long -32,0,-28,0
#else
  .long -2,0,-28,0
#endif
#endif
  .long FILTER_SENTINEL,.Louter_filter,.Louter_handler
#if REGISTRATION_CASE == 41 || REGISTRATION_CASE == 42
  .long 0,.Lmiddle_filter,.Lmiddle_handler
  .long 1,.Lskipped_filter,.Lskipped_handler
  .long 0,.Lfilter,.Lhandler
  .long 3,0,.Lfinally
#else
  .long 0,.Lfilter,.Lhandler
  .long 1,0,.Lfinally
#endif
#if REGISTRATION_CASE == 54 || FILTER_REPEATED_JUMP
  .long 1,0,.Lselected_cleanup
#elif REGISTRATION_CASE == 56 || FILTER_NESTED_SEARCH
  .long 1,0,.Lselected_nested
#endif
#if FILTER_NESTED_JUMP
  .long 1,0,.Lcleanup_jump
#endif
  .long 0,0,0
.globl __load_config_used
__load_config_used:
  .long 92
#if REGISTRATION_EH4_FILTER
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

#if REGISTRATION_EH4_FILTER
#include "registration_cookie_check.inc"
#endif

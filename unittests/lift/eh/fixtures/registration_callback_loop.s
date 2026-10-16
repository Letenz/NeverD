// Bounded loops in a suspended finally or its search filter. The underlying
// state/capture protocol is the shared-state fixture; only loop storage varies.
#if REGISTRATION_CASE >= 79
#define REGISTRATION_LOOP_IN_FILTER 1
#endif
#if REGISTRATION_CASE == 77 || REGISTRATION_CASE == 80
#define REGISTRATION_LOOP_STORAGE 1
#elif REGISTRATION_CASE == 78 || REGISTRATION_CASE == 81
#define REGISTRATION_LOOP_STORAGE 2
#else
#define REGISTRATION_LOOP_STORAGE 0
#endif
#define REGISTRATION_EH4_FILTER REGISTRATION_LOOP_EH4
#undef REGISTRATION_CASE
#define REGISTRATION_CASE 37

.macro registration_bounded_loop
#if REGISTRATION_LOOP_STORAGE == 0
  mov ecx,3
#elif REGISTRATION_LOOP_STORAGE == 1
  mov dword ptr [ebp-104],3
#else
  sub esp,4
  mov dword ptr [esp],3
#endif
.Lbounded_callback_loop\@:
  mov eax,[ebp-100]
  imul eax,eax,10
  add eax,6
  mov [ebp-100],eax
#if REGISTRATION_LOOP_STORAGE == 0
  dec ecx
#elif REGISTRATION_LOOP_STORAGE == 1
  dec dword ptr [ebp-104]
#else
  dec dword ptr [esp]
#endif
  jnz .Lbounded_callback_loop\@
#if REGISTRATION_LOOP_STORAGE == 2
  add esp,4
#endif
.endm

#define REGISTRATION_HAS_CALLBACK_LOOP 1
#include "registration_filter_state.s"

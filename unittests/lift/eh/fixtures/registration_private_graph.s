// The retained helper graph has its own stack, no source frame argument and
// one observable effect. A live setjmp buffer must survive the complete call.
.intel_syntax noprefix
.text
.globl _registration_private_graph
_registration_private_graph:
  call .Lprivate_leaf
  mov eax,7
  ret
.Lprivate_leaf:
  add dword ptr [_registration_helper_visits],1
  mov eax,3
  ret
.def @feat.00;
.scl 3;
.type 0;
.endef
.set @feat.00,1

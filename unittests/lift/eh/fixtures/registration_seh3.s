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
#define REGISTRATION_SENTINEL -1
#include "registration_seh_body.inc"
.safeseh __except_handler3
.section .rdata,"dr"
.p2align 2
.Lscope_table:
#include "registration_seh_scopes.inc"
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

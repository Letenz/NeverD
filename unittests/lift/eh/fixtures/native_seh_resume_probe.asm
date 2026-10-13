;===- native_seh_resume_probe.asm - Independent x64 ABI observations ------===;
;
; NeverD Decompiler
;
;===--------------------------------------------------------------------===;
; Physical ABI oracle only, not an emitted continuation implementation.

EXTERN neverd_native_seh_resume:PROC
PUBLIC neverd_native_resume_probe

SaveXMM MACRO Reg, Offset
  movdqa xmmword ptr [rsp + Offset], Reg
  .savexmm128 Reg, Offset
ENDM

CheckGPR MACRO Reg, Value
  LOCAL Equal
  cmp Reg, Value
  je Equal
  mov dword ptr [r10 + 4], 0
Equal:
ENDM

CheckXMM MACRO Reg
  LOCAL Equal
  movdqa xmm0, Reg
  pcmpeqb xmm0, xmmword ptr [RegisterPattern]
  pmovmskb eax, xmm0
  cmp eax, 0ffffh
  je Equal
  mov dword ptr [r10 + 4], 0
Equal:
ENDM

.data
ALIGN 16
RegisterPattern DQ 0123456789abcdef0h, 0fedcba9876543210h

.code
neverd_native_resume_probe PROC FRAME
  push rbp
  .pushreg rbp
  push rbx
  .pushreg rbx
  push rsi
  .pushreg rsi
  push rdi
  .pushreg rdi
  push r12
  .pushreg r12
  push r13
  .pushreg r13
  push r14
  .pushreg r14
  push r15
  .pushreg r15
  sub rsp, 0d8h
  .allocstack 0d8h
  SaveXMM xmm6, 20h
  SaveXMM xmm7, 30h
  SaveXMM xmm8, 40h
  SaveXMM xmm9, 50h
  SaveXMM xmm10, 60h
  SaveXMM xmm11, 70h
  SaveXMM xmm12, 80h
  SaveXMM xmm13, 90h
  SaveXMM xmm14, 0a0h
  SaveXMM xmm15, 0b0h
  .endprolog
  mov qword ptr [rsp + 0c0h], r8
  mov qword ptr [rsp + 0c8h], rsp
  mov dword ptr [rsp + 0d0h], 51a7e001h
  mov rbp, 13570001h
  mov rbx, 13570002h
  mov rsi, 13570003h
  mov rdi, 13570004h
  mov r12, 13570005h
  mov r13, 13570006h
  mov r14, 13570007h
  mov r15, 13570008h
  movdqa xmm6, xmmword ptr [RegisterPattern]
  movdqa xmm7, xmm6
  movdqa xmm8, xmm6
  movdqa xmm9, xmm6
  movdqa xmm10, xmm6
  movdqa xmm11, xmm6
  movdqa xmm12, xmm6
  movdqa xmm13, xmm6
  movdqa xmm14, xmm6
  movdqa xmm15, xmm6
  call neverd_native_seh_resume
  mov r10, qword ptr [rsp + 0c0h]
  mov dword ptr [r10], eax
  mov dword ptr [r10 + 4], 1
  CheckGPR rbp, 13570001h
  CheckGPR rbx, 13570002h
  CheckGPR rsi, 13570003h
  CheckGPR rdi, 13570004h
  CheckGPR r12, 13570005h
  CheckGPR r13, 13570006h
  CheckGPR r14, 13570007h
  CheckGPR r15, 13570008h
  CheckXMM xmm6
  CheckXMM xmm7
  CheckXMM xmm8
  CheckXMM xmm9
  CheckXMM xmm10
  CheckXMM xmm11
  CheckXMM xmm12
  CheckXMM xmm13
  CheckXMM xmm14
  CheckXMM xmm15
  xor eax, eax
  cmp rsp, qword ptr [rsp + 0c8h]
  sete al
  mov dword ptr [r10 + 8], eax
  xor eax, eax
  cmp dword ptr [rsp + 0d0h], 51a7e001h
  sete al
  mov dword ptr [r10 + 12], eax
  mov eax, dword ptr [r10]
  movdqa xmm6, xmmword ptr [rsp + 20h]
  movdqa xmm7, xmmword ptr [rsp + 30h]
  movdqa xmm8, xmmword ptr [rsp + 40h]
  movdqa xmm9, xmmword ptr [rsp + 50h]
  movdqa xmm10, xmmword ptr [rsp + 60h]
  movdqa xmm11, xmmword ptr [rsp + 70h]
  movdqa xmm12, xmmword ptr [rsp + 80h]
  movdqa xmm13, xmmword ptr [rsp + 90h]
  movdqa xmm14, xmmword ptr [rsp + 0a0h]
  movdqa xmm15, xmmword ptr [rsp + 0b0h]
  add rsp, 0d8h
  pop r15
  pop r14
  pop r13
  pop r12
  pop rdi
  pop rsi
  pop rbx
  pop rbp
  ret
neverd_native_resume_probe ENDP
END

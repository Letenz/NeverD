// The canonical leaf CRT success path is observable only on a bad cookie.
.text
.p2align 4,0x90
.globl "@__security_check_cookie@4"
"@__security_check_cookie@4":
  cmp ecx,dword ptr [___security_cookie]
  jne .Lcookie_failure
  ret
.Lcookie_failure:
  push 99
  call dword ptr ["__imp__ExitProcess@4"]
  int3

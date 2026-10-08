// Strict cookie oracle for Wine, whose EH4 dispatcher omits cookie checks.
// The common runtime still owns dispatch, unwind and callback invocation.
typedef unsigned word;
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
typedef void(__fastcall *cookie_check)(word);
__declspec(dllimport) int __cdecl
_except_handler4_common(word *, cookie_check, void *, void *, void *, void *);

word __security_cookie = 0x51b39d27;
word registration_corrupt_cookie;

static void __fastcall check_cookie(word value) {
  if (value != __security_cookie)
    ExitProcess(99);
}

void __fastcall __security_check_cookie(word value);

#if !REGISTRATION_FORWARD_ONLY
int __cdecl _except_handler4(void *exception, word *registration, void *context,
                             void *dispatcher) {
  word *table = (word *)(registration[2] ^ __security_cookie);
  char *runtime_frame = (char *)registration + 16;
  if ((table[0] != (word)-2) != REGISTRATION_EXPECT_GS)
    ExitProcess(98);
  if (registration_corrupt_cookie == 1)
    *(word *)(runtime_frame + (int)table[2]) ^= 1;
  if (registration_corrupt_cookie == 2 && table[0] != (word)-2)
    *(word *)(runtime_frame + (int)table[0]) ^= 1;
  if (table[0] != (word)-2)
    check_cookie(*(word *)(runtime_frame + (int)table[0]) ^
                 (word)(runtime_frame + (int)table[1]));
  check_cookie(*(word *)(runtime_frame + (int)table[2]) ^
               (word)(runtime_frame + (int)table[3]));
  return _except_handler4_common(&__security_cookie, __security_check_cookie,
                                 exception, registration, context, dispatcher);
}
#endif

// Functions that take their parameters in registers, as GCC passes a local
// function's first three (regparm: EAX, EDX, ECX), and the rest on the
// stack: the debug information says where each enters, and the decompiler
// names each after the source parameter there, not after cdecl's slots.

__attribute__((regparm(3), noinline)) int
dp32_regs(const unsigned char *buf, int len, int bias, int extra) {
  return bias * 3 - buf[len] + extra;
}

// An unused parameter has no location: the stack slot of the one after it
// is not certain.
__attribute__((regparm(3), noinline)) int dp32_skip(int unused, int len,
                                                    int bias, int extra) {
  return bias * 5 - len + extra;
}

int dp32_call_regs(const unsigned char *buf) {
  return dp32_regs(buf, 3, 7, 11) + dp32_skip(1, 2, 3, 4);
}

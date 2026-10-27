// A result established before a branch around a store, which joins again at
// the return: every path returns that result, so the function is no void one.
// AArch64 and ARM keep the call's result in the return register across the
// branch.  The i386, ARM and AArch64 fixtures include this file.
// The callee writes memory the store may alias, so the store stays after the
// call instead of becoming a tail call's prelude.
int calls;
__attribute__((noinline)) int triple(int x) {
  ++calls;
  return x * 3;
}

int call_then_store(int *p, int v) {
  int r = triple(v);
  if (v)
    *p = v;
  return r;
}

// A bare return beside a tail call.  AArch64 and ARM pass the first argument
// in the result register, so the path that returns it writes nothing there;
// x86 moves it.
extern long g(long);
long (*fp)(long);

long passthrough_direct(long a, long b) {
  if (b)
    return g(b);
  return a;
}

long passthrough_indirect(long a, long b) {
  if (b)
    return fp(b);
  return a;
}

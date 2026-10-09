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

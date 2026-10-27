// A MinGW-w64 image that calls two functions through `__imp_X` variables, as
// code compiled for a DLL import calls through its slot.  MinGW's runtime
// defines such variables for the functions it supplies in place of an import
// (__MINGW_IMP_SYMBOL), initialized to the function; a lazy binding starts at
// a function that rewrites its variable.
__attribute__((noinline)) static int add_one(int x) { return x + 1; }
int (*__imp_add_one)(int) = add_one;

__attribute__((noinline)) static int twice(int x) { return x + x; }
static int bind_twice(int x);
int (*__imp_twice)(int) = bind_twice;
__attribute__((noinline)) static int bind_twice(int x) {
  __imp_twice = twice;
  return twice(x);
}

int entry(void) { return __imp_add_one(1) + __imp_twice(2); }

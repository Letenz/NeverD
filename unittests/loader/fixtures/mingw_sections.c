// A MinGW-w64 image whose COFF symbol table GNU ld fills as it does for any
// program: a static symbol for each input section an object contributes, at
// the address of its first function or variable, a .file record, and the
// program's own static and external names.
int counter = 7;
static int hidden_total;

__attribute__((noinline)) static int twice(int x) { return x + x; }

int bump(int x) {
  hidden_total += x;
  return counter + twice(x);
}

int entry(void) { return bump(1); }

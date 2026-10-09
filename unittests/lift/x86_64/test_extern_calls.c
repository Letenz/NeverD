// Functions and data a relocatable object names but does not define, a
// tentative definition it leaves common (-fcommon), and a defined global its
// position-independent code reaches through the GOT.  Each call, data
// reference and GOT reference must reach the symbol of that name, not address
// zero, where first() sits.  The i386, ARM and AArch64 fixtures include this
// file.
extern void perror(const char *);
extern void exit(int) __attribute__((noreturn));
extern int fputs(const char *, void *);
extern void *stdout;
extern int counter;
int defined_global = 3;
int tentative;

int first(void) { return 7; }

__attribute__((noreturn)) void die(int code) {
  perror("probe");
  exit(code);
}

int put(const char *s) { return fputs(s, stdout); }

void bump(void) { counter += 5; }

int read_defined(void) { return defined_global; }

int read_tentative(void) { return tentative; }

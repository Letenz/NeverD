// Functions and data a COFF object names but does not define, the pointer an
// import library supplies to a function it calls (`__imp_`), and a tentative
// definition it leaves common (-fcommon).  Each must reach the symbol of that
// name, not an address before every section.
extern void perror(const char *);
extern __declspec(noreturn) void exit(int);
extern int fputs(const char *, void *);
extern void *stream;
extern int counter;
__declspec(dllimport) int __stdcall ImportedApi(int);
int tentative;

int first(void) { return 7; }

__declspec(noreturn) void die(int code) {
  perror("probe");
  exit(code);
}

int put(const char *s) { return fputs(s, stream); }

void bump(void) { counter += 5; }

int call_api(int x) { return ImportedApi(x) + 1; }

int read_tentative(void) { return tentative; }

// A MinGW-w64 image that keeps a table of function pointers among its code,
// headed by -1 as GNU ld heads __CTOR_LIST__ in .text, and reaches it through
// a relocated pointer stored in .data.  Its COFF symbol table names the table
// with no function type.
typedef int (*handler_t)(int);

__attribute__((noinline)) static int twice(int x) { return x + x; }

__attribute__((section(".text$table"), used))
const handler_t text_table[3] = {(handler_t)-1, twice, 0};

const handler_t *table_ref = text_table;

int call_second(int x) { return table_ref[1](x); }

int entry(void) { return call_second(1); }

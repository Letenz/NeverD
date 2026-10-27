// Unions whose alternatives are as wide as one another, which code reads
// and writes by the alternative its access's class names.  Regenerate the
// object with:
//   gcc -O1 -g -c dwarf_unions.c -o dwarf_unions.gcc.o

union value {
  long long i;
  double d;
};

struct holder {
  int tag;
  union value v;
};

long long get_i(union value *v) { return v->i; }

double get_d(union value *v) { return v->d; }

void set_i(union value *v, long long x) { v->i = x; }

void set_d(union value *v, double x) { v->d = x; }

long long held_i(struct holder *h) { return h->v.i; }

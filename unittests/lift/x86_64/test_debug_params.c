// Debug signatures whose parameters the System V convention passes out of
// signature order: integer values in integer registers, floating values in
// vector registers, a 16-byte record in two registers and a complex number
// in two vector registers.  The decompiler names each recovered parameter
// after the source parameter the convention put there.

struct Pair {
  long a, b;
};

double dp_scale(double x, int n) { return x * n + n; }

long dp_pair_sum(struct Pair p, long k) { return p.a * 3 + p.b * 5 + k; }

double dp_complex_mix(_Complex double z, int n) {
  return __real__ z * n + __imag__ z;
}

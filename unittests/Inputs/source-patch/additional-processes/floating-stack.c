
extern double original_fp(float, double, int, int, int, int, int, int, int, int);
int replacement(int x) {
  return (int)original_fp(2.5f, 3.5, x, 2, 3, 4, 5, 6, 7, 8);
}


int _fltused = 0;
__declspec(dllexport) __declspec(noinline) int original_helper(int x) {
  volatile int y = x;
  return y + 1;
}
__declspec(dllexport) __declspec(noinline) double original_fp(
    float a, double b, int x, int c, int d, int e, int f, int g, int h, int i) {
  return a * 2 + b + x + c + d + e + f + g + h + i;
}
__declspec(dllexport) __declspec(noinline) int target(int x) {
  return original_helper(x);
}
int entry(void) {
  if (original_helper(4) != 5) return 250;
  int a = target(2);
  int b = target(5);
  if (original_helper(9) != 10) return 251;
  return a + b;
}

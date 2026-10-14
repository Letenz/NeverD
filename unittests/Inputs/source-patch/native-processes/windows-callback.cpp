
extern "C" int cleanup_count;
using Callback = int (*)(int);
extern "C" __declspec(noinline) int throwing(int x) { if (x) throw x; return 0; }
extern "C" __declspec(dllexport) __declspec(noinline) int target(Callback cb, int x) {
  volatile int value = cb(x); return value + 1;
}
int main(int argc, char **argv) {
  int x = argc > 1 ? argv[1][0] - '0' : 0;
  try { return target(throwing, x); }
  catch (int value) { return value + 40; }
}

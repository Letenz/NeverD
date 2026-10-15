
extern "C" __declspec(dllimport) void _exit(int);
using terminate_handler = void (*)();
extern "C" __declspec(dllimport) terminate_handler set_terminate(terminate_handler);
extern "C" __declspec(dllexport) int cleanup_count = 0;
struct Guard { ~Guard() { ++cleanup_count; } };
extern "C" __declspec(dllexport) __declspec(noinline) int original_thrower(int x) {
  if (x) throw x; return 0;
}
extern "C" __declspec(dllexport) __declspec(noinline) int target(int x) {
  volatile int value = x; if (value == -999) throw value; return value + 1;
}
__declspec(noinline) int reraiser(int x) {
  try { return original_thrower(x); } catch (...) { Guard g; throw; }
}
extern "C" __declspec(dllexport) __declspec(noinline) int original_noexcept(int x) noexcept {
  return original_thrower(x);
}
void on_terminate() { _exit(77); }
int main(int argc, char **argv) {
  set_terminate(on_terminate);
  int x = argc > 1 ? argv[1][0] - '0' : 0;
  if (x == 8) return original_noexcept(x);
  try { if (x == 9) return reraiser(9); return target(x); }
  catch (int value) { return value + cleanup_count * 10; }
}

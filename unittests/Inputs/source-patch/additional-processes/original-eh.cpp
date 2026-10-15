
extern "C" { int cleanup_count = 0; void _Exit(int); }
struct Guard { ~Guard() { ++cleanup_count; } };
extern "C" __attribute__((noinline)) int original_thrower(int x) { if (x) throw x; return x; }
// The original call site must allow an exception; otherwise the compiler is
// entitled to erase its catch/cleanup path, which a replacement cannot restore.
extern "C" __attribute__((noinline)) int target(int x) {
  volatile int y=x; if (y == -999) throw y; return y+1;
}
__attribute__((noinline)) int reraiser(int x) {
  try { return original_thrower(x); } catch (...) { Guard g; throw; }
}
namespace std {
  using terminate_handler = void (*)();
  terminate_handler set_terminate(terminate_handler) noexcept;
  void terminate() noexcept;
}
void on_terminate() { _Exit(77); }
int main(int argc, char **argv) {
  std::set_terminate(on_terminate);
  int x=argc>1 ? argv[1][0]-'0' : 0;
  if (x==8) std::terminate();
  try { if (x==9) return reraiser(9); return target(x); }
  catch (int value) { return value + cleanup_count*10; }
}

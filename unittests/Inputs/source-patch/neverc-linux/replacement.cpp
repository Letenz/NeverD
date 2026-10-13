
template <class T> T twice(T x) { return x * 2; }
struct Guard {
  int *p;
  Guard(int *q) : p(q) { *p += 3; }
  ~Guard() { *p += 5; }
};
extern "C" int replacement(int x) {
  int value = x;
  { Guard guard(&value); value = twice(value); }
  return value;
}
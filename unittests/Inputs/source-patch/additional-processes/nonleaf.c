
extern int original_helper(int);
int replacement(int x) {
  volatile int y = original_helper(x);
  return y + 39;
}


extern int original_helper(int);
static volatile int counter = 40;
static volatile int *volatile pointer = &counter;
int replacement(int x) {
  ++*pointer;
  return *pointer + original_helper(x);
}

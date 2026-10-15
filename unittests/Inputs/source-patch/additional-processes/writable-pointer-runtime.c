
extern int original_helper(int);
static volatile int counter = 40;
static volatile int *volatile pointer;
int replacement(int x) {
  if (!pointer) pointer = &counter;
  ++*pointer;
  return *pointer + original_helper(x);
}

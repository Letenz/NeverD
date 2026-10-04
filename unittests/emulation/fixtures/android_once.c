/* Independently authored single-thread Android initialization workloads. */
typedef unsigned int u32;
typedef unsigned long u64;
extern int pthread_once(u32 *, void (*)(void));
extern u32 getuid(void);
extern int mprotect(void *, u64, int);
extern void unknown_initializer(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static u32 constructor_control;
static u32 constructor_count;
static void initialize_constructor(void) { ++constructor_count; }
__attribute__((constructor)) static void construct(void) {
  pthread_once(&constructor_control, initialize_constructor);
  pthread_once(&constructor_control, initialize_constructor);
}

struct observed {
  u32 outer, inner, outer_count, inner_count;
  u32 saw_outer, saw_inner, inner_after, uid;
  u32 constructors, constructor_state, stack_ok, unused[5];
};
static struct observed *current;
static void initialize_inner(void) {
  ++current->inner_count;
  current->saw_inner = current->inner;
  current->uid = getuid();
}
static void initialize_outer(void) {
  ++current->outer_count;
  current->saw_outer = current->outer;
  pthread_once(&current->inner, initialize_inner);
  current->inner_after = current->inner;
}
u64 once_values(struct observed *output) {
  volatile u64 live[3] = {0x123456789abcdef0UL, 0xfedcba9876543210UL,
                          0x8765432112345678UL};
  current = output;
  pthread_once(&output->outer, initialize_outer);
  pthread_once(&output->outer, initialize_outer);
  output->constructors = constructor_count;
  output->constructor_state = constructor_control;
  output->stack_ok = live[0] == 0x123456789abcdef0UL &&
                     live[1] == 0xfedcba9876543210UL &&
                     live[2] == 0x8765432112345678UL;
  return 73;
}
u64 once_supplied(u32 *control, u64 initializer) {
  return (u32)pthread_once(control, (void (*)(void))initializer);
}
u64 once_dynamic(struct observed *output) {
  void *library = dlopen("libinit.so", 2);
  if (!library)
    return 98;
  int (*initialize)(u32 *, void (*)(void)) = dlsym(library, "pthread_once");
  if (!initialize)
    return 99;
  current = output;
  initialize(&output->outer, initialize_outer);
  initialize(&output->outer, initialize_outer);
  return (u32)dlclose(library);
}
u64 once_readonly(u32 *control, u32 state) {
  *control = state;
  if (mprotect((void *)((u64)control & ~(u64)4095), 4096, 1))
    return 99;
  return (u32)pthread_once(control, initialize_constructor);
}

static void initialize_recursive(void) {
  ++current->outer_count;
  pthread_once(&current->outer, initialize_recursive);
}
u64 once_recursive(struct observed *output) {
  current = output;
  return (u32)pthread_once(&output->outer, initialize_recursive);
}
static void initialize_unknown(void) { unknown_initializer(); }
static void initialize_nested_failure(void) {
  pthread_once(&current->inner, initialize_unknown);
}
u64 once_fails(struct observed *output) {
  current = output;
  return (u32)pthread_once(&output->outer, initialize_nested_failure);
}
static void initialize_loop(void) {
  volatile u32 *count = &current->outer_count;
  for (;;)
    ++*count;
}
u64 once_exhausts(struct observed *output) {
  current = output;
  return (u32)pthread_once(&output->outer, initialize_loop);
}
__attribute__((naked)) static void initialize_bad_stack(void) {
  __asm__ volatile("sub sp, sp, #16\nret");
}
u64 once_bad_stack(u32 *control) {
  return (u32)pthread_once(control, initialize_bad_stack);
}

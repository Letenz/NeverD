/* Independently authored Android C++ destruction ABI workloads. */
typedef unsigned long u64;
typedef unsigned int u32;
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern void __cxa_finalize(void *);
extern int pthread_once(u32 *, void (*)(void));
extern int *__errno(void);
extern u32 getuid(void);
extern long syscall(long, ...);
extern void unknown_finalizer(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);

static char first_dso, second_dso;
static u64 constructors;
static u64 *observed;
static u32 once_control;

static void constructor_destructor(void *argument) { ++*(u64 *)argument; }
__attribute__((constructor)) static void construct(void) {
  __cxa_atexit(constructor_destructor, &constructors, &first_dso);
}
static void record(void *argument) {
  u64 count = observed[0]++;
  observed[8 + count] = (u64)argument;
  observed[6] = getuid();
}
u64 cxa_sequence(u64 *output) {
  volatile u64 live[3] = {0x123456789abcdef0UL, 0xfedcba9876543210UL,
                          0x8765432112345678UL};
  observed = output;
  *__errno() = 73;
  __cxa_atexit(record, (void *)11, &first_dso);
  __cxa_atexit(record, (void *)22, &second_dso);
  __cxa_atexit(record, (void *)33, &first_dso);
  __cxa_atexit(record, (void *)44, 0);
  __cxa_atexit(record, (void *)33, &first_dso);
  __cxa_atexit(0, (void *)-1UL, &first_dso);
  __cxa_finalize(&first_dso);
  output[1] = output[0];
  __cxa_finalize(&first_dso);
  __cxa_finalize((void *)0x123456789abcdef1UL);
  output[2] = output[0];
  __cxa_finalize(0);
  output[3] = output[0];
  __cxa_atexit(record, (void *)55, &second_dso);
  __cxa_finalize(&second_dso);
  __cxa_finalize(0);
  output[4] = constructors;
  output[5] = live[0] == 0x123456789abcdef0UL &&
              live[1] == 0xfedcba9876543210UL &&
              live[2] == 0x8765432112345678UL;
  output[7] = (u32)*__errno();
  return 73;
}

static void initialize(void) {
  record((void *)66);
  __cxa_atexit(record, (void *)77, &first_dso);
  __cxa_finalize(&first_dso);
}
static void recursive(void *argument) {
  record(argument);
  pthread_once(&once_control, initialize);
  __cxa_atexit(record, (void *)88, &second_dso);
  __cxa_finalize(&first_dso);
  record((void *)99);
}
u64 cxa_nested(u64 *output) {
  observed = output;
  __cxa_atexit(record, (void *)11, &first_dso);
  __cxa_atexit(record, (void *)22, &second_dso);
  __cxa_atexit(recursive, (void *)33, &first_dso);
  __cxa_finalize(&first_dso);
  __cxa_finalize(0);
  output[1] = constructors;
  output[2] = once_control;
  return 73;
}

static void append(void *argument) {
  record(argument);
  __cxa_atexit(record, (void *)33, &first_dso);
  __cxa_atexit(record, (void *)44, &second_dso);
}
u64 cxa_append(u64 *output) {
  observed = output;
  __cxa_atexit(record, (void *)11, &first_dso);
  __cxa_atexit(append, (void *)22, &first_dso);
  __cxa_finalize(&first_dso);
  output[1] = output[0];
  __cxa_finalize(0);
  return 73;
}

u64 cxa_supplied(u64 entry, u64 argument, u64 dso, u64 finalize) {
  int result =
      __cxa_atexit((void (*)(void *))entry, (void *)argument, (void *)dso);
  if (finalize)
    __cxa_finalize((void *)dso);
  return (u32)result;
}
u64 cxa_arguments(u64 *output, u64 argument, u64 dso) {
  observed = output;
  __cxa_atexit(record, (void *)argument, (void *)dso);
  __cxa_finalize((void *)dso);
  return 73;
}
static void fail(void *argument) {
  record(argument);
  unknown_finalizer();
}
static void loop(void *argument) {
  record(argument);
  volatile u64 *count = observed + 1;
  for (;;)
    ++*count;
}
static void terminate(void *argument) {
  record(argument);
  syscall(94, 19);
}
__attribute__((naked)) static void bad_stack(void *argument) {
  __asm__ volatile("sub sp, sp, #16\nret");
}
u64 cxa_stop(u64 *output, u64 mode) {
  observed = output;
  __cxa_atexit(record, (void *)11, &first_dso);
  void (*function)(void *) = mode == 0   ? fail
                             : mode == 1 ? loop
                             : mode == 2 ? bad_stack
                                         : terminate;
  __cxa_atexit(function, (void *)33, &first_dso);
  __cxa_finalize(&first_dso);
  return 99;
}
u64 cxa_register_only(void) {
  __cxa_atexit(fail, 0, &first_dso);
  return 73;
}
u64 cxa_capacity(u64 count, u64 passes) {
  for (u64 pass = 0; pass < passes; ++pass) {
    for (u64 i = 0; i < count; ++i)
      if (__cxa_atexit(0, 0, 0))
        return 98;
    __cxa_finalize(0);
  }
  return 73;
}
u64 cxa_dynamic(u64 *output, u64 closed) {
  observed = output;
  void *library = dlopen("libfinalize-model.so", 2);
  if (!library)
    return 98;
  int (*add)(void (*)(void *), void *, void *) = dlsym(library, "__cxa_atexit");
  void (*finish)(void *) = dlsym(library, "__cxa_finalize");
  if (!add || !finish ||
      add(record, (void *)0x100000009UL, (void *)0x123456789abcdef1UL))
    return 99;
  if (closed && dlclose(library))
    return 97;
  finish((void *)0x123456789abcdef1UL);
  if (!closed && dlclose(library))
    return 96;
  return 73;
}

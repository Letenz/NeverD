/* Original freestanding callers of the public JNI 1.6 LP64 table ABI. */
typedef unsigned long long U64;
typedef unsigned char U8;
typedef unsigned short U16;
typedef const U64 *Table;
typedef Table *Handle;
typedef U64 Ref;
extern int JNI_GetCreatedJavaVMs(Handle *, int, int *);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
#define C(E, R, S, ...) (((R (*)(Handle, ...))(*E)[S])(E, ##__VA_ARGS__))
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X))                                                                  \
      return __LINE__;                                                         \
  } while (0)
static Handle vm;
static Handle attach(void) {
  int count = 0;
  Handle env = 0;
  if (JNI_GetCreatedJavaVMs(&vm, 1, &count) || count != 1 ||
      C(vm, int, 4, &env, (void *)0))
    return 0;
  return env;
}
int jni_vm_lifecycle(void) {
  int count = 7;
  CHECK(!JNI_GetCreatedJavaVMs((void *)0, 0, &count) && count == 0);
  CHECK(!JNI_GetCreatedJavaVMs(&vm, 1, &count));
  CHECK(count == 1);
  U64 aliased = 0;
  CHECK(!JNI_GetCreatedJavaVMs((Handle *)&aliased, 1, (int *)&aliased));
  CHECK(aliased == (U64)vm);
  Handle env = (Handle)123;
  CHECK(C(vm, int, 6, &env, 0x10006) == -2 && !env);
  env = (Handle)123;
  CHECK(C(vm, int, 6, &env, 0x10008) == -3 && env == (Handle)123);
  CHECK(!C(vm, int, 4, &env, (void *)0) && env);
  Handle first = env;
  CHECK(C(env, int, 4) == 0x10006);
  CHECK(!C(vm, int, 7, &env, (void *)1) && env == first);
  CHECK(!C(vm, int, 6, &env, 0x10001) && env == first);
  Handle owner = 0;
  CHECK(!C(env, int, 219, &owner) && owner == vm);
  CHECK(!C(vm, int, 5));
  CHECK(C(vm, int, 5) == -1);
  CHECK(!C(vm, int, 4, &env, (void *)0) && env != first);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_string_values(void) {
  Handle e = attach();
  CHECK(e);
  const char text[] = "A\300\200\355\240\275\355\270\200";
  Ref s = C(e, Ref, 167, text);
  CHECK(s);
  CHECK(C(e, int, 164, s) == 4 && C(e, int, 168, s) == 9);
  U8 copy = 0;
  const volatile U16 *p = C(e, const U16 *, 165, s, &copy);
  CHECK(copy == 1 && p[0] == 65 && p[1] == 0 && p[2] == 0xd83d &&
        p[3] == 0xde00);
  Ref same = C(e, Ref, 163, p, 4);
  CHECK(same && same != s);
  C(e, void, 166, s, p);
  const volatile U8 *q = C(e, const U8 *, 169, same, &copy);
  for (unsigned i = 0; i < sizeof(text); ++i)
    CHECK(q[i] == (U8)text[i]);
  C(e, void, 170, same, q);
  volatile U8 region[8];
  for (unsigned i = 0; i < 8; ++i)
    region[i] = 0x7d;
  C(e, void, 221, s, 1, 2, region);
  CHECK(region[0] == 0xc0 && region[1] == 0x80 && region[2] == 0xed &&
        region[3] == 0xa0 && region[4] == 0xbd && region[5] == 0x7d);
  C(e, void, 220, s, -1, 1, region);
  CHECK(C(e, U8, 228) == 1 && region[0] == 0xc0);
  Ref error = C(e, Ref, 15);
  CHECK(error);
  C(e, void, 17);
  Ref kind = C(e, Ref, 6, "java/lang/StringIndexOutOfBoundsException");
  CHECK(C(e, U8, 32, error, kind) == 1);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_arrays(void) {
  Handle e = attach();
  CHECK(e);
  Ref b = C(e, Ref, 176, 4);
  CHECK(b && C(e, int, 171, b) == 4);
  U8 input[] = {0x80, 2, 3, 4};
  C(e, void, 208, b, 0, 4, input);
  U8 copy = 0;
  volatile U8 *p = C(e, U8 *, 184, b, &copy);
  CHECK(copy && p[0] == 0x80);
  p[1] = 9;
  C(e, void, 192, b, p, 1);
  volatile U8 output[4] = {0};
  C(e, void, 200, b, 0, 4, output);
  CHECK(output[1] == 9);
  p[1] = 77;
  C(e, void, 192, b, p, 2);
  C(e, void, 200, b, 0, 4, output);
  CHECK(output[1] == 9 && output[0] == 0x80);
  C(e, void, 208, b, 3, 2, input);
  CHECK(C(e, U8, 228));
  C(e, void, 17);
  CHECK(!C(e, Ref, 176, -1) && C(e, U8, 228));
  C(e, void, 17);
  Ref string = C(e, Ref, 167, "value");
  Ref cls = C(e, Ref, 6, "java/lang/String");
  Ref array = C(e, Ref, 172, 2, cls, string);
  Ref element = C(e, Ref, 173, array, 1);
  CHECK(array && C(e, U8, 24, element, string));
  C(e, void, 174, array, 1, b);
  CHECK(C(e, U8, 228));
  C(e, void, 17);
  element = C(e, Ref, 173, array, 1);
  CHECK(C(e, U8, 24, element, string));
  C(e, void, 174, array, 1, (Ref)0);
  CHECK(!C(e, Ref, 173, array, 1));
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_references(void) {
  Handle e = attach();
  CHECK(e);
  CHECK(!C(e, int, 19, 8));
  Ref s = C(e, Ref, 167, "retained");
  Ref global = C(e, Ref, 21, s);
  CHECK(global != s && C(e, int, 232, global) == 2);
  Ref promoted = C(e, Ref, 20, s);
  CHECK(promoted != s && C(e, int, 232, promoted) == 1);
  CHECK(C(e, U8, 24, global, promoted));
  C(e, void, 23, promoted);
  CHECK(!C(vm, int, 5));
  e = attach();
  CHECK(e);
  CHECK(C(e, int, 164, global) == 8);
  C(e, void, 22, global);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_primitive_bits(void) {
  Handle e = attach();
  CHECK(e);
  const unsigned widths[] = {1, 1, 2, 2, 4, 8, 4, 8};
  volatile U8 input[24], output[24];
  for (unsigned kind = 0; kind < 8; ++kind) {
    unsigned bytes = widths[kind] * 3;
    for (unsigned i = 0; i < bytes; ++i) {
      input[i] = kind ? (U8)(i * 67 + 0x81) : (i & 1);
      output[i] = 0x55;
    }
    Ref array = C(e, Ref, 175 + kind, 3);
    CHECK(array);
    C(e, void, 207 + kind, array, 0, 3, input);
    C(e, void, 199 + kind, array, 0, 3, output);
    for (unsigned i = 0; i < bytes; ++i)
      CHECK(input[i] == output[i]);
    U8 copy = 0;
    volatile U8 *p = C(e, U8 *, 183 + kind, array, &copy);
    CHECK(copy);
    p[0] = 0;
    C(e, void, 191 + kind, array, p, 0);
    C(e, void, 199 + kind, array, 0, 3, output);
    CHECK(!output[0]);
    for (unsigned i = 1; i < bytes; ++i)
      CHECK(input[i] == output[i]);
    C(e, void, 23, array);
  }
  CHECK(!C(vm, int, 5));
  return 0;
}
static Handle parent_env;
static Ref shared;
static void *thread_value(void *arg) {
  Handle e = 0;
  if (C(vm, int, 6, &e, 0x10006) != -2 || e)
    return (void *)1;
  if (C(vm, int, 4, &e, (void *)0) || e == parent_env)
    return (void *)2;
  if (arg)
    return (void *)(U64)C(parent_env, int, 4);
  if (C(e, int, 164, shared) != 6)
    return (void *)3;
  return (void *)(U64)C(vm, int, 5);
}
int jni_threads(void) {
  parent_env = attach();
  CHECK(parent_env);
  Ref local = C(parent_env, Ref, 167, "shared");
  shared = C(parent_env, Ref, 21, local);
  U64 thread;
  void *result = (void *)99;
  CHECK(!pthread_create(&thread, 0, thread_value, 0));
  CHECK(!pthread_join(thread, &result) && !result);
  C(parent_env, void, 22, shared);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_wrong_thread(void) {
  parent_env = attach();
  CHECK(parent_env);
  U64 thread;
  CHECK(!pthread_create(&thread, 0, thread_value, (void *)1));
  pthread_join(thread, 0);
  return 0;
}
int jni_stale_env(void) {
  Handle e = attach();
  C(vm, int, 5);
  return C(e, int, 4);
}
int jni_stale_ref(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "gone");
  C(e, void, 23, s);
  return C(e, int, 164, s);
}
int jni_unknown_method(void) {
  Handle e = attach();
  Ref cls = C(e, Ref, 6, "java/lang/String");
  return (int)C(e, Ref, 33, cls, "length", "()I");
}
int jni_invalid_utf8(void) {
  Handle e = attach();
  return (int)C(e, Ref, 167, "\360\237\230\200");
}
int jni_double_release(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "once");
  const char *p = C(e, const char *, 169, s, (void *)0);
  C(e, void, 170, s, p);
  C(e, void, 170, s, p);
  return 0;
}

int jni_frame_underflow(void) {
  Handle e = attach();
  return (int)C(e, Ref, 20, (Ref)0);
}
int jni_detach_borrow(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "live");
  C(e, const char *, 169, s, (void *)0);
  return C(vm, int, 5);
}
int jni_wrong_borrow_object(void) {
  Handle e = attach();
  Ref a = C(e, Ref, 176, 4), b = C(e, Ref, 176, 4);
  void *p = C(e, void *, 184, a, (void *)0);
  C(e, void, 192, b, p, 0);
  return 0;
}
int jni_wrong_reference_kind(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "local");
  C(e, void, 22, s);
  return 0;
}
int jni_pending_cleanup(void) {
  Handle e = attach();
  CHECK(e);
  Ref array = C(e, Ref, 176, 1);
  U8 *p = C(e, U8 *, 184, array, (void *)0);
  CHECK(p);
  p[0] = 0x8a;
  Ref failed = C(e, Ref, 176, -1);
  CHECK(!failed && C(e, U8, 228));
  CHECK(!C(e, int, 19, 2));
  Ref error = C(e, Ref, 15);
  CHECK(error);
  Ref saved = C(e, Ref, 20, error);
  CHECK(saved && C(e, U8, 228));
  C(e, void, 192, array, p, 0);
  C(e, void, 17);
  volatile U8 byte = 0;
  C(e, void, 200, array, 0, 1, &byte);
  CHECK(byte == 0x8a && !C(e, U8, 228));
  C(e, void, 23, saved);
  CHECK(!C(vm, int, 5));
  return 0;
}
int jni_pending_illegal(void) {
  Handle e = attach();
  C(e, Ref, 176, -1);
  return C(e, int, 4);
}
int jni_table_replaced(void) {
  extern int mprotect(void *, U64, int);
  Handle e = attach();
  U64 page = (U64)e & ~(U64)4095;
  CHECK(!mprotect((void *)page, 4096, 3));
  *(volatile U64 *)((U64)e + 8) = 0;
  return C(e, int, 4);
}
int jni_invalid_copyout(void) {
  Handle e = attach();
  Ref s = C(e, Ref, 167, "guard");
  return (int)(U64)C(e, const char *, 169, s, (void *)1);
}

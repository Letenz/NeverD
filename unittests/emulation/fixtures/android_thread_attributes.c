/* Independent Android API 28 LP64 callers; no host pthread implementation. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef struct {
  u32 flags;
  void *stack_base;
  u64 stack_size;
  u64 guard_size;
  int sched_policy;
  int sched_priority;
  char reserved[16];
} thread_attr;
typedef struct {
  int sched_priority;
} sched_param;
_Static_assert(sizeof(thread_attr) == 56, "LP64 attribute size");
_Static_assert(_Alignof(thread_attr) == 8, "LP64 attribute alignment");
_Static_assert(__builtin_offsetof(thread_attr, stack_base) == 8, "stack base");
_Static_assert(__builtin_offsetof(thread_attr, stack_size) == 16, "stack size");
_Static_assert(__builtin_offsetof(thread_attr, guard_size) == 24, "guard size");
_Static_assert(__builtin_offsetof(thread_attr, sched_policy) == 32, "policy");
_Static_assert(__builtin_offsetof(thread_attr, sched_priority) == 36,
               "priority");
_Static_assert(__builtin_offsetof(thread_attr, reserved) == 40, "reserved");
extern int pthread_attr_init(thread_attr *);
extern int pthread_attr_destroy(thread_attr *);
extern int pthread_attr_setdetachstate(thread_attr *, int);
extern int pthread_attr_getdetachstate(const thread_attr *, int *);
extern int pthread_attr_setinheritsched(thread_attr *, int);
extern int pthread_attr_getinheritsched(const thread_attr *, int *);
extern int pthread_attr_setschedpolicy(thread_attr *, int);
extern int pthread_attr_getschedpolicy(const thread_attr *, int *);
extern int pthread_attr_setschedparam(thread_attr *, const sched_param *);
extern int pthread_attr_getschedparam(const thread_attr *, sched_param *);
extern int pthread_attr_setstacksize(thread_attr *, u64);
extern int pthread_attr_getstacksize(const thread_attr *, u64 *);
extern int pthread_attr_setguardsize(thread_attr *, u64);
extern int pthread_attr_getguardsize(const thread_attr *, u64 *);
extern int pthread_attr_setstack(thread_attr *, void *, u64);
extern int pthread_attr_getstack(const thread_attr *, void **, u64 *);
extern int pthread_attr_setscope(thread_attr *, int);
extern int pthread_attr_getscope(const thread_attr *, int *);
extern int pthread_attr_unknown(thread_attr *);
extern int pthread_getattr_np(u64, thread_attr *);
extern int pthread_create(u64 *, const thread_attr *, void *(*)(void *),
                          void *);
extern int *__errno(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, u64, int);

u64 thread_attribute_call(thread_attr *attr, u64 operation, u64 value,
                          u64 extra) {
  int *error = __errno();
  *error = 77;
  int result;
  switch (operation) {
  case 0:
    result = pthread_attr_init(attr);
    break;
  case 1:
    result = pthread_attr_destroy(attr);
    break;
  case 2:
    result = pthread_attr_setdetachstate(attr, (int)value);
    break;
  case 3:
    result = pthread_attr_getdetachstate(attr, (int *)value);
    break;
  case 4:
    result = pthread_attr_setinheritsched(attr, (int)value);
    break;
  case 5:
    result = pthread_attr_getinheritsched(attr, (int *)value);
    break;
  case 6:
    result = pthread_attr_setschedpolicy(attr, (int)value);
    break;
  case 7:
    result = pthread_attr_getschedpolicy(attr, (int *)value);
    break;
  case 8:
    result = pthread_attr_setschedparam(attr, (const sched_param *)value);
    break;
  case 9:
    result = pthread_attr_getschedparam(attr, (sched_param *)value);
    break;
  case 10:
    result = pthread_attr_setstacksize(attr, value);
    break;
  case 11:
    result = pthread_attr_getstacksize(attr, (u64 *)value);
    break;
  case 12:
    result = pthread_attr_setguardsize(attr, value);
    break;
  case 13:
    result = pthread_attr_getguardsize(attr, (u64 *)value);
    break;
  case 14:
    result = pthread_attr_setstack(attr, (void *)value, extra);
    break;
  case 15:
    result = pthread_attr_getstack(attr, (void **)value, (u64 *)extra);
    break;
  case 16:
    result = pthread_attr_setscope(attr, (int)value);
    break;
  case 17:
    result = pthread_attr_getscope(attr, (int *)value);
    break;
  case 18:
    result = pthread_attr_unknown(attr);
    break;
  case 19:
    result = pthread_getattr_np(1000, attr);
    break;
  default:
    result = pthread_create((u64 *)value, attr, 0, 0);
    break;
  }
  return *error == 77 ? (u32)result : ~0UL;
}

u64 thread_attribute_readonly(thread_attr *attr, u64 operation, void *page) {
  if (mprotect(page, 4096, 1))
    return 99;
  return thread_attribute_call(attr, operation, 0, 0);
}

u64 thread_attribute_dynamic(u64 *out, u64 close_first) {
  thread_attr attr;
  unsigned char *bytes = (unsigned char *)&attr;
  for (u64 i = 0; i < sizeof(attr); ++i)
    bytes[i] = 0xa5;
  void *handle = dlopen("libthread-model.so", 2);
  if (!handle)
    return 1;
  int (*init)(thread_attr *) =
      (int (*)(thread_attr *))dlsym(handle, "pthread_attr_init");
  int (*size)(const thread_attr *, u64 *) =
      (int (*)(const thread_attr *, u64 *))dlsym(handle,
                                                 "pthread_attr_getstacksize");
  out[0] = (u64)init;
  out[1] = (u64)size;
  if (!init || !size)
    return 2;
  if (close_first && dlclose(handle))
    return 3;
  if (init(&attr) || size(&attr, out + 2))
    return 4;
  bytes = (unsigned char *)&attr;
  for (u64 i = 0; i < sizeof(attr); ++i)
    ((unsigned char *)(out + 3))[i] = bytes[i];
  return close_first ? 0 : (u32)dlclose(handle);
}

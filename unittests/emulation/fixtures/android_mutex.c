/* Independently authored callers for the Android 9 LP64 pthread ABI. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef long mutex_attr;
typedef struct {
  int opaque[10];
} mutex_object;
_Static_assert(sizeof(mutex_attr) == 8, "LP64 attribute");
_Static_assert(sizeof(mutex_object) == 40, "LP64 object");
_Static_assert(_Alignof(mutex_object) == 4, "mutex alignment");
extern int pthread_mutexattr_init(mutex_attr *);
extern int pthread_mutexattr_destroy(mutex_attr *);
extern int pthread_mutexattr_settype(mutex_attr *, int);
extern int pthread_mutexattr_gettype(const mutex_attr *, int *);
extern int pthread_mutexattr_setpshared(mutex_attr *, int);
extern int pthread_mutexattr_getpshared(const mutex_attr *, int *);
extern int pthread_mutexattr_setprotocol(mutex_attr *, int);
extern int pthread_mutexattr_getprotocol(const mutex_attr *, int *);
extern int pthread_mutex_init(mutex_object *, const mutex_attr *);
extern int pthread_mutex_destroy(mutex_object *);
extern int pthread_mutex_lock(mutex_object *);
extern int pthread_mutex_trylock(mutex_object *);
extern int pthread_mutex_unlock(mutex_object *);
extern int *__errno(void);
extern int gettid(void);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, u64, int);

static void fill(void *destination, unsigned char value, u64 size) {
  unsigned char *p = destination;
  for (u64 i = 0; i < size; ++i)
    p[i] = value;
}
static void copy(void *destination, const void *source, u64 size) {
  unsigned char *d = destination;
  const unsigned char *s = source;
  for (u64 i = 0; i < size; ++i)
    d[i] = s[i];
}

static mutex_object boot_mutex = {{0x4000}};
static volatile u64 constructor_value;
__attribute__((constructor)) static void mutex_constructor(void) {
  constructor_value = 99;
  if (pthread_mutex_lock(&boot_mutex) || pthread_mutex_lock(&boot_mutex) ||
      pthread_mutex_unlock(&boot_mutex) || pthread_mutex_unlock(&boot_mutex))
    return;
  constructor_value = 73;
}
u64 mutex_constructor_result(void) { return constructor_value; }

u64 mutex_attributes(unsigned char *out) {
  struct {
    u64 before;
    mutex_attr attribute;
    u64 after;
  } storage;
  struct {
    int value;
    u32 guard;
  } output = {-1, 0xa1b2c3d4};
  fill(&storage, 0xa5, sizeof(storage));
  *__errno() = 77;
  if (pthread_mutexattr_init(&storage.attribute))
    return 1;
  copy(out, &storage.attribute, 8);
  if (storage.attribute || pthread_mutexattr_settype(&storage.attribute, 1) ||
      pthread_mutexattr_setpshared(&storage.attribute, 1) ||
      pthread_mutexattr_setprotocol(&storage.attribute, 1))
    return 2;
  copy(out + 8, &storage.attribute, 8);
  if (storage.attribute != 0x31 ||
      pthread_mutexattr_gettype(&storage.attribute, &output.value) ||
      output.value != 1 ||
      pthread_mutexattr_getpshared(&storage.attribute, &output.value) ||
      output.value != 1 ||
      pthread_mutexattr_getprotocol(&storage.attribute, &output.value) ||
      output.value != 1 || output.guard != 0xa1b2c3d4)
    return 3;
  if (pthread_mutexattr_settype(&storage.attribute, -1) != 22 ||
      pthread_mutexattr_setpshared(&storage.attribute, 2) != 22 ||
      pthread_mutexattr_setprotocol(&storage.attribute, 2) != 22 ||
      storage.attribute != 0x31)
    return 4;
  storage.attribute = 0x0123456789abcdefL;
  if (pthread_mutexattr_gettype(&storage.attribute, (int *)1) != 22 ||
      pthread_mutexattr_settype((mutex_attr *)1, -1) != 22 ||
      pthread_mutexattr_setpshared((mutex_attr *)1, 2) != 22 ||
      pthread_mutexattr_setprotocol((mutex_attr *)1, -1) != 22)
    return 5;
  if (pthread_mutexattr_settype(&storage.attribute, 2) ||
      pthread_mutexattr_setpshared(&storage.attribute, 0) ||
      pthread_mutexattr_setprotocol(&storage.attribute, 0) ||
      storage.attribute != 0x0123456789abcdc2L)
    return 6;
  copy(out + 16, &storage.attribute, 8);
  if (pthread_mutexattr_destroy(&storage.attribute) || storage.attribute != -1)
    return 7;
  copy(out + 24, &storage.attribute, 8);
  if (storage.before != 0xa5a5a5a5a5a5a5a5UL ||
      storage.after != storage.before || *__errno() != 77)
    return 8;
  return 0;
}

u64 mutex_sequence(unsigned char *out, u64 type, u64 shared) {
  struct {
    u32 before;
    mutex_object object;
    u32 after;
  } storage;
  mutex_attr attr;
  fill(&storage, 0xa5, sizeof(storage));
  *__errno() = 77;
  if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr, type) ||
      pthread_mutexattr_setpshared(&attr, shared) ||
      pthread_mutex_init(&storage.object, type || shared ? &attr : 0))
    return 1;
  copy(out, &storage.object, 40);
  int no_owner = pthread_mutex_unlock(&storage.object);
  if (no_owner != (type ? 1 : 0) || pthread_mutex_lock(&storage.object))
    return 2;
  copy(out + 40, &storage.object, 40);
  int tried = pthread_mutex_trylock(&storage.object);
  if (tried != (type == 1 ? 0 : 16))
    return 3;
  copy(out + 80, &storage.object, 40);
  if (type == 2 && pthread_mutex_lock(&storage.object) != 35)
    return 4;
  if (pthread_mutex_destroy(&storage.object) != 16 ||
      pthread_mutex_unlock(&storage.object))
    return 5;
  if (type == 1 && pthread_mutex_unlock(&storage.object))
    return 6;
  copy(out + 120, &storage.object, 40);
  if (pthread_mutex_destroy(&storage.object))
    return 7;
  copy(out + 160, &storage.object, 40);
  if (pthread_mutexattr_destroy(&attr) || storage.before != 0xa5a5a5a5 ||
      storage.after != 0xa5a5a5a5 || *__errno() != 77)
    return 8;
  return 0;
}

u64 mutex_recursive_limit(unsigned char *out) {
  /* A valid guest-owned recursive lock one step below its maximum depth. */
  mutex_object object = {{0x5ff9}};
  object.opaque[1] = gettid();
  *__errno() = 77;
  if (pthread_mutex_trylock(&object))
    return 1;
  copy(out, &object, 40);
  if (pthread_mutex_trylock(&object) != 11 || pthread_mutex_lock(&object) != 11)
    return 2;
  copy(out + 40, &object, 40);
  if (pthread_mutex_unlock(&object) || pthread_mutex_unlock(&object))
    return 3;
  copy(out + 80, &object, 40);
  return *__errno() == 77 ? 0 : 4;
}

u64 mutex_initialization_order(unsigned char *out) {
  union {
    mutex_object object;
    u64 words[5];
  } storage;
  mutex_attr invalid = 3;
  fill(&storage, 0xa5, sizeof(storage));
  if (pthread_mutex_init(&storage.object, &invalid) != 22)
    return 1;
  copy(out, &storage, 40);
  fill(&storage, 0xa5, sizeof(storage));
  storage.words[1] = 1;
  if (pthread_mutex_init(&storage.object, (mutex_attr *)&storage.words[1]))
    return 2;
  copy(out + 40, &storage, 40);
  return 0;
}

u64 mutex_object_call(mutex_object *object, u64 operation, mutex_attr *attr) {
  switch (operation) {
  case 0:
    return pthread_mutex_lock(object);
  case 1:
    return pthread_mutex_trylock(object);
  case 2:
    return pthread_mutex_unlock(object);
  case 3:
    return pthread_mutex_destroy(object);
  case 4:
    return pthread_mutex_init(object, attr);
  default:
    return 99;
  }
}
u64 mutex_attribute_call(mutex_attr *attribute, u64 operation, u64 argument) {
  switch (operation) {
  case 0:
    return pthread_mutexattr_init(attribute);
  case 1:
    return pthread_mutexattr_gettype(attribute, (int *)argument);
  case 2:
    return pthread_mutexattr_settype(attribute, argument);
  default:
    return 99;
  }
}

u64 mutex_dynamic(u64 *out, u64 closed) {
  typedef int (*mutex_fn)(mutex_object *);
  mutex_object object = {{0x8000}};
  void *library = dlopen("libpthread-model.so", 2);
  if (!library)
    return 1;
  mutex_fn lock = (mutex_fn)dlsym(library, "pthread_mutex_lock");
  mutex_fn unlock = (mutex_fn)dlsym(library, "pthread_mutex_unlock");
  if (!lock || !unlock)
    return 2;
  out[0] = (u64)lock;
  out[1] = (u64)unlock;
  if (closed) {
    if (dlclose(library))
      return 3;
    return lock(&object);
  }
  if (lock(&object) || unlock(&object) || dlclose(library))
    return 4;
  out[2] = gettid();
  return 0;
}

u64 mutex_readonly_tail(mutex_object *object) {
  u64 page = ((u64)object + 4095) & ~(u64)4095;
  if (mprotect((void *)page, 4096, 1))
    return 99;
  return pthread_mutex_lock(object);
}

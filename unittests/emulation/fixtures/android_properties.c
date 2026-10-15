/* Independently authored, freestanding Android property API callers. */
typedef unsigned long u64;
typedef unsigned int u32;
typedef struct prop_info prop_info;
extern const prop_info *__system_property_find(const char *);
extern const prop_info *__system_property_find_nth(unsigned);
extern const prop_info *
    property_nth_wide(u64) __asm__("__system_property_find_nth");
extern int __system_property_get(const char *, char *);
extern int __system_property_read(const prop_info *, char *, char *);
extern u32 __system_property_serial(const prop_info *);
extern void __system_property_read_callback(const prop_info *,
                                            void (*)(void *, const char *,
                                                     const char *, u32),
                                            void *);
extern int __system_property_foreach(void (*)(const prop_info *, void *),
                                     void *);
extern int *__errno(void);
extern int pthread_once(u32 *, void (*)(void));
extern int pthread_create(u64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int mprotect(void *, u64, int);
extern int munmap(void *, u64);
extern void unknown_property_callback(void);

static u64 hash(const char *s) {
  u64 h = 0;
  while (*s)
    h = h * 131 + (unsigned char)*s++;
  return h;
}

static void observed_value(void *cookie, const char *name, const char *value,
                           u32 serial) {
  u64 *out = cookie;
  out[0]++;
  out[1] = hash(name);
  out[2] = hash(value);
  out[3] = serial;
  out[4] = __system_property_serial(__system_property_find(name));
}

static void observed_property(const prop_info *pi, void *cookie) {
  u64 *out = cookie;
  u64 i = out[0]++;
  char name[32], value[92];
  u64 *row = out + 8 + i * 8;
  row[0] = __system_property_read(pi, name, value);
  row[1] = hash(name);
  row[2] = hash(value);
  row[3] = pi == __system_property_find_nth((unsigned)i);
  __system_property_read_callback(pi, observed_value, row + 3);
}

u64 property_sequence(u64 *out) {
  volatile u64 live[] = {0x123456789abcdef0UL, 0xfedcba9876543210UL};
  *__errno() = 719;
  out[1] = __system_property_foreach(observed_property, out);
  out[2] = __system_property_find("absent") == 0;
  out[3] = __system_property_find_nth((unsigned)out[0]) == 0;
  out[4] = *__errno();
  out[5] = live[0];
  out[6] = live[1];
  return out[0];
}

u64 property_read_supplied(const char *key, char *name, char *value) {
  const prop_info *pi = __system_property_find(key);
  return (u32)__system_property_read(pi, name, value);
}
u64 property_read_handle(const prop_info *pi, char *name, char *value) {
  return (u32)__system_property_read(pi, name, value);
}
u64 property_find_supplied(const char *key) {
  return (u64)__system_property_find(key);
}
u64 property_find_nth(u64 index) { return (u64)property_nth_wide(index); }
u64 property_serial_handle(const prop_info *pi) {
  return __system_property_serial(pi);
}
u64 property_callback_supplied(const char *key, u64 *out) {
  const prop_info *pi = __system_property_find(key);
  __system_property_read_callback(pi, observed_value, out);
  return 0;
}
u64 property_foreach_supplied(void (*callback)(const prop_info *, void *),
                              void *cookie) {
  return (u32)__system_property_foreach(callback, cookie);
}
u64 property_callback_address(const char *key, u64 callback, u64 cookie) {
  __system_property_read_callback(__system_property_find(key), (void *)callback,
                                  (void *)cookie);
  return 0;
}

static void count_property(const prop_info *pi, void *cookie) {
  u64 *out = cookie;
  ++out[0];
  out[1] += __system_property_serial(pi);
}
static u32 once_control;
static u64 *once_output;
static void once_body(void) {
  __system_property_foreach(count_property, once_output + 2);
}
static void nested_property(const prop_info *pi, void *cookie) {
  u64 *out = cookie;
  ++out[0];
  __system_property_read_callback(pi, observed_value, out + 8);
  __system_property_foreach(count_property, out + 4);
  pthread_once(&once_control, once_body);
}
u64 property_nested(u64 *out) {
  once_output = out;
  out[1] = __system_property_foreach(nested_property, out);
  return 0;
}

static void *thread_body(void *cookie) {
  u64 *out = cookie;
  *__errno() = (int)out[15];
  out[13] = __system_property_foreach(count_property, out);
  __system_property_read_callback(__system_property_find("test.alpha"),
                                  observed_value, out + 8);
  out[14] = *__errno();
  return (void *)out[0];
}
u64 property_threads(u64 *out) {
  u64 child;
  void *returned = 0;
  out[15] = 101;
  out[31] = 202;
  if (pthread_create(&child, 0, thread_body, out + 16))
    return 1;
  thread_body(out);
  if (pthread_join(child, &returned))
    return 2;
  return (u64)returned;
}

u64 property_dynamic(u64 *out, u64 close_first) {
  void *library = dlopen("libproperties.so", 2);
  if (!library)
    return 98;
  const prop_info *(*find)(const char *) =
      dlsym(library, "__system_property_find");
  int (*read)(const prop_info *, char *, char *) =
      dlsym(library, "__system_property_read");
  void (*callback)(const prop_info *,
                   void (*)(void *, const char *, const char *, u32), void *) =
      dlsym(library, "__system_property_read_callback");
  int (*each)(void (*)(const prop_info *, void *), void *) =
      dlsym(library, "__system_property_foreach");
  if (!find || !read || !callback || !each)
    return 97;
  const prop_info *pi = find("test.alpha");
  out[7] = pi == __system_property_find("test.alpha");
  if (close_first && dlclose(library))
    return 96;
  char value[92];
  out[5] = read(pi, 0, value);
  out[6] = hash(value);
  callback(pi, observed_value, out);
  each(count_property, out + 8);
  if (!close_first)
    return (u32)dlclose(library);
  return 0;
}

static void failing_property(const prop_info *pi, void *cookie) {
  u64 mode = (u64)cookie;
  if (!mode)
    unknown_property_callback();
  if (mode == 1)
    for (;;)
      __asm__ volatile("" ::: "memory");
  if (mode == 2)
    __system_property_foreach(failing_property, cookie);
  (void)pi;
}
u64 property_stop(u64 mode) {
  return __system_property_foreach(failing_property, (void *)mode);
}

u64 property_tamper(const char *key, u64 mode) {
  const prop_info *pi = __system_property_find(key);
  void *page = (void *)((u64)pi & ~4095UL);
  if (mode == 0) {
    *(volatile char *)pi = 7;
  } else if (mode == 1) {
    if (munmap(page, 4096))
      return 97;
  } else {
    if (mprotect(page, 4096, 3))
      return 98;
    *(volatile char *)pi = 7;
  }
  char value[92];
  return __system_property_read(pi, 0, value);
}

u64 property_get_supplied(const char *key, char *value) {
  return __system_property_get(key, value);
}

u64 property_read_protected(const char *key, char *value, void *page) {
  if (mprotect(page, 4096, 1))
    return 98;
  return __system_property_read(__system_property_find(key), 0, value);
}

u64 property_read_image_output(const char *key, u64 use_get) {
  const prop_info *pi = __system_property_find(key);
  if (mprotect((void *)((u64)pi & ~4095UL), 4096, 3))
    return 98;
  if (use_get)
    return __system_property_get(key, (char *)pi + 96);
  char name[32];
  return __system_property_read(pi, name, (char *)pi + 96);
}

static void full_cookie(void *cookie, const char *name, const char *value,
                        u32 serial) {
  (void)name;
  (void)value;
  (void)serial;
  if ((u64)cookie != 0xfedcba9876543210UL)
    unknown_property_callback();
  *__errno() = 811;
}
u64 property_cookie(void) {
  *__errno() = 719;
  __system_property_read_callback(__system_property_find("test.alpha"),
                                  full_cookie, (void *)0xfedcba9876543210UL);
  return (unsigned)*__errno();
}

__attribute__((naked)) static void unbalanced(const prop_info *pi,
                                              void *cookie) {
  __asm__ volatile("sub sp, sp, #16\nret\n");
}
u64 property_unbalanced(void) {
  return __system_property_foreach(unbalanced, 0);
}

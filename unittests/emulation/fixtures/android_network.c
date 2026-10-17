/* Original API 28 ABI callers. No host headers or network dependencies. */
typedef unsigned long u64;
typedef unsigned int u32;
struct address_info {
  int flags, family, socket_type, protocol;
  u32 length;
  char *canonical;
  void *address;
  struct address_info *next;
};
_Static_assert(sizeof(struct address_info) == 48, "Bionic LP64 addrinfo");
_Static_assert(__builtin_offsetof(struct address_info, address) == 32,
               "address offset");
extern int getaddrinfo(const char *, const char *, const struct address_info *,
                       struct address_info **);
extern void freeaddrinfo(struct address_info *);
extern int *__errno(void);
extern int socket(int, int, int);
extern int connect(int, const void *, u32);
extern long send(int, const void *, u64, int);
extern long recv(int, void *, u64, int);
extern int close(int);
extern int dup(int);
extern long write(int, const void *, u64);
extern long read(int, void *, u64);
extern long syscall(long, ...);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

u64 network_resolve(u64 *out, u64 mode) {
  struct address_info hints = {0}, *result = (void *)0x1234;
  hints.flags = 2;
  hints.socket_type = 1;
  if (mode == 1)
    hints.flags = 0x80000000u;
  if (mode == 2)
    hints.family = 999;
  if (mode == 3)
    hints.length = 1;
  const char *node = mode == 4 ? 0 : "node.invalid";
  const char *service = mode == 4 ? 0 : "9001";
  *__errno() = 73;
  out[0] = getaddrinfo(node, service, &hints, &result);
  out[1] = *__errno();
  out[2] = (u64)result;
  if (out[0])
    return 0;
  unsigned n = 0;
  for (struct address_info *p = result; p && n < 4; p = p->next, ++n) {
    u64 *q = out + 4 + n * 16;
    q[0] = p->flags;
    q[1] = p->family;
    q[2] = p->socket_type;
    q[3] = p->protocol;
    q[4] = p->length;
    unsigned char *b = (void *)(q + 5);
    for (unsigned i = 0; i != p->length && i < 28; ++i)
      b[i] = ((unsigned char *)p->address)[i];
    b = (void *)(q + 9);
    if (p->canonical)
      for (unsigned i = 0; i < 40; ++i) {
        b[i] = p->canonical[i];
        if (!b[i])
          break;
      }
  }
  out[3] = n;
  freeaddrinfo(result);
  freeaddrinfo(0);
  out[68] = *__errno();
  if (mode == 5)
    freeaddrinfo(result);
  return 0;
}

u64 network_stream(u64 *out, u64 raw) {
  struct address_info hints = {0}, *result = 0;
  hints.socket_type = 1;
  hints.flags = 2;
  if (getaddrinfo("node.invalid", "9001", &hints, &result))
    return 1;
  int fd =
      raw ? syscall(198, result->family, 1, 0) : socket(result->family, 1, 0);
  if (fd < 0)
    return 2;
  int r = raw ? syscall(203, fd, result->address, result->length)
              : connect(fd, result->address, result->length);
  out[0] = (unsigned)r;
  out[1] = *__errno();
  freeaddrinfo(result);
  if (r)
    return close(fd);
  int alias = dup(fd);
  if (alias < 0 || close(fd))
    return 3;
  const unsigned char input[5] = {0, 0xff, 0x42, 0x13, 0x7f};
  out[2] = send(alias, input, 5, 0x4000);
  out[3] = write(alias, input + 2, 3);
  out[4] = raw ? syscall(206, alias, input + 4, 1, 0, 0, 0)
               : send(alias, input + 4, 1, 0);
  out[5] = recv(alias, (unsigned char *)(out + 16) + 1, 8, 0);
  out[6] = read(alias, (unsigned char *)(out + 16) + 3, 8);
  out[7] = recv(alias, (unsigned char *)(out + 16) + 5, 8, 0);
  out[8] = close(alias);
  out[9] = connect(alias, (void *)1, 16);
  out[10] = *__errno();
  return 0;
}

u64 network_dynamic(u64 *out) {
  void *lib = dlopen("liboffline.so", 2);
  if (!lib)
    return 1;
  int (*resolve)(const char *, const char *, const struct address_info *,
                 struct address_info **) = dlsym(lib, "getaddrinfo");
  void (*release)(struct address_info *) = dlsym(lib, "freeaddrinfo");
  struct address_info hints = {0}, *p = 0;
  hints.flags = 2;
  hints.socket_type = 1;
  out[0] = resolve("node.invalid", "9001", &hints, &p);
  if (!out[0]) {
    out[1] = p->family;
    release(p);
  }
  return 0;
}

extern const char *inet_ntop(int, const void *, char *, u32);
u64 network_ntop(u64 *out, u64 mode) {
  int family = (u32)mode;
  char *dst = (char *)(out + 8);
  *__errno() = 73;
  const char *p = inet_ntop(family, family == 999 ? 0 : out + 64,
                          family == 999 ? 0 : dst, mode >> 32);
  out[0] = p == dst;
  out[1] = *__errno();
  return 0;
}

extern int setsockopt(int, int, int, const void *, u32);
extern int getsockopt(int, int, int, void *, u32 *);
u64 network_timeout(u64 *out, u64 raw) {
  int fd = socket(2, 1, 6), alias = dup(fd);
  long time[2] = {20, 0};
  *__errno() = 73;
  out[0] = raw ? syscall(208, fd, 1, 20, time, 16)
               : setsockopt(fd, 1, 20, time, 16);
  close(fd);
  u32 size = 24;
  out[1] = raw ? syscall(209, alias, 1, 20, out + 4, &size)
               : getsockopt(alias, 1, 20, out + 4, &size);
  out[2] = size;
  out[3] = *__errno();
  close(alias);
  return 0;
}

extern int inet_pton(int, const char *, void *);
u64 network_pton(u64 *out, u64 family) {
  *__errno() = 73;
  out[0] = inet_pton(family, family == 999 ? 0 : (char *)(out + 64),
                     family == 999 ? 0 : out + 8);
  out[1] = *__errno();
  return 0;
}

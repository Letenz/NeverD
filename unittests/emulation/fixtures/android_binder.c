/* Independent callers of the Linux LP64 Binder protocol-8 control ABI. */
typedef unsigned long long U64;
extern int open(const char *, int, ...);
extern int close(int);
extern int dup(int);
extern int ioctl(int, unsigned long, void *);
extern int *__errno(void);
extern int pthread_create(U64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(U64, void **);
struct WriteRead {
  U64 size, consumed, data, read_size, read_consumed, read_data;
};
#define CHECK(X)                                                               \
  do {                                                                         \
    if (!(X))                                                                  \
      return __LINE__;                                                         \
  } while (0)
static int exchange(int fd, struct WriteRead *wr) {
  return ioctl(fd, 0xc0306201UL, wr);
}
int binder_control(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  unsigned version[3] = {0xaabbccdd, 99, 0xaabbccdd};
  CHECK(!ioctl(fd, 0xc0046209UL, &version[1]));
  CHECK(version[0] == 0xaabbccdd && version[1] == 8 &&
        version[2] == 0xaabbccdd);
  CHECK(ioctl(fd, 0xc0046209UL, (void *)1) == -1 && *__errno() == 22);
  CHECK(ioctl(fd, 0x40046205UL, (void *)1) == -1 && *__errno() == 22);
  unsigned max = 1;
  CHECK(!ioctl(fd, 0x40046205UL, &max));
  unsigned commands[] = {0x630c, 0x630d};
  unsigned output[3] = {0x88776655, 0x88776655, 0x88776655};
  struct WriteRead wr = {8, 0, (U64)commands, 8, 0, (U64)output};
  CHECK(exchange(fd, &wr) == -1 && *__errno() == 11);
  CHECK(wr.consumed == 8 && !wr.read_consumed && output[0] == 0x720c);
  CHECK(output[1] == 0x88776655 && output[2] == 0x88776655);
  CHECK(!ioctl(fd, 0x40046208UL, (void *)1));
  wr = (struct WriteRead){4, 0, (U64)commands, 8, 0, (U64)output};
  CHECK(!exchange(fd, &wr));
  CHECK(wr.consumed == 4 && wr.read_consumed == 4 && output[0] == 0x720d);
  commands[0] = 0x630b;
  wr = (struct WriteRead){4, 0, (U64)commands, 0, 0, 1};
  CHECK(!exchange(fd, &wr) && wr.consumed == 4);
  max = 0xffffffff;
  CHECK(!ioctl(fd, 0x40046205UL, &max));
  CHECK(!ioctl(fd, 0x40046208UL, (void *)1));
  commands[0] = 0x630c;
  wr = (struct WriteRead){4, 0, (U64)commands, 8, 0, (U64)output};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 4 && output[0] == 0x720c);
  CHECK(!close(fd));
  return 0;
}
int binder_descriptions(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  unsigned word = 0x99;
  struct WriteRead wr = {0, 0, 1, 4, 0, (U64)&word};
  CHECK(!exchange(fd, &wr) && wr.read_consumed == 4 && word == 0x720c);
  int alias = dup(fd), other = open("/dev/binder", 2 | 0x800);
  CHECK(alias >= 0 && other >= 0);
  CHECK(!close(fd));
  wr.read_consumed = 0;
  CHECK(exchange(alias, &wr) == -1 && *__errno() == 11 && !wr.read_consumed);
  CHECK(!exchange(other, &wr) && wr.read_consumed == 4);
  CHECK(!ioctl(alias, 0x40046208UL, (void *)1));
  wr.read_consumed = 0;
  CHECK(!exchange(alias, &wr) && wr.read_consumed == 4);
  CHECK(!close(alias) && !close(other));
  return 0;
}
static void *child(void *arg) {
  int fd = (int)(U64)arg;
  unsigned word = 0;
  struct WriteRead wr = {0, 0, 1, 4, 0, (U64)&word};
  if (exchange(fd, &wr) || wr.read_consumed != 4 || word != 0x720c)
    return (void *)1;
  return (void *)(U64)ioctl(fd, 0x40046208UL, 0);
}
int binder_threads(void) {
  int fd = open("/dev/binder", 2 | 0x800);
  CHECK(fd >= 0);
  unsigned version = 0;
  CHECK(!ioctl(fd, 0xc0046209UL, &version));
  U64 id = 0;
  void *result = (void *)99;
  CHECK(!pthread_create(&id, 0, child, (void *)(U64)fd));
  CHECK(!pthread_join(id, &result) && !result);
  unsigned word = 0;
  struct WriteRead wr = {0, 0, 0, 4, 0, (U64)&word};
  CHECK(exchange(fd, &wr) == -1 && *__errno() == 11 && !wr.read_consumed);
  CHECK(!close(fd));
  return 0;
}
int binder_unknown_transaction(void) {
  int fd = open("/dev/binder", 2);
  unsigned command = 0x40406300;
  struct WriteRead wr = {4, 0, (U64)&command, 0, 0, 0};
  return exchange(fd, &wr);
}
int binder_blocking(void) {
  int fd = open("/dev/binder", 2);
  unsigned word = 0;
  CHECK(!ioctl(fd, 0xc0046209UL, &word));
  struct WriteRead wr = {0, 0, 0, 4, 0, (U64)&word};
  return exchange(fd, &wr);
}
int binder_partial_command(void) {
  int fd = open("/dev/binder", 2);
  unsigned command = 0x630c;
  struct WriteRead wr = {3, 0, (U64)&command, 0, 0, 0};
  return exchange(fd, &wr);
}

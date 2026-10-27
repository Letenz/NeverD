/* Independently authored Android LP64 callers; all peers are virtual inputs. */
typedef unsigned long U;
extern long syscall(long, ...);
extern int *__errno(void);
extern int socket(int, int, int);
extern int connect(int, const void *, unsigned);
extern int close(int);
extern int mprotect(void *, U, int);
extern long recv(int, void *, U, int);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
extern int nanosleep(const void *, void *);
extern int clock_gettime(int, void *);

static long raw_select(U n, U *set, U *time) {
  register U x0 __asm__("x0") = n;
  register U *x1 __asm__("x1") = set;
  register U x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  register U *x4 __asm__("x4") = time;
  register U x5 __asm__("x5") = 0, x8 __asm__("x8") = 72;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                   : "memory", "cc");
  return (long)x0;
}
struct State {
  U *out, *set, *time;
  U route, action;
  int fd;
};
static void *reader(void *arg) {
  struct State *s = arg;
  *__errno() = 73;
  s->out[0] = s->route ? syscall(72, s->fd + 1, s->set, (void *)0, (void *)0,
                                 s->time, (void *)0)
                       : raw_select(s->fd + 1, s->set, s->time);
  s->out[1] = (unsigned)*__errno();
  clock_gettime(1, s->out + 2);
  if (s->out[0] == 1)
    s->out[4] = recv(s->fd, s->out + 5, 1, 0);
  s->out[6] = 123;
  return (void *)42;
}
static void *other(void *arg) {
  struct State *s = arg;
  U delay[2] = {0, 3};
  if (nanosleep(delay, 0))
    return (void *)1;
  clock_gettime(1, s->out + 8);
  if (s->action == 1) {
    s->set[0] = 0;
    s->time[0] = 0;
    s->time[1] = 99;
  } else if (s->action == 2) {
    if (mprotect(s->set, 4096, 1))
      return (void *)2;
  } else if (s->action == 3) {
    if (close(s->fd))
      return (void *)3;
  }
  s->out[10] = 456;
  return (void *)43;
}
U readiness_workers(U *out, U route, U action) {
  const unsigned char peer[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
  int fd = socket(2, 1, 6);
  if (fd < 0 || fd >= 64 || connect(fd, peer, 16))
    return 1;
  U *set = out + 512, *time = out + 16;
  set[0] = 1UL << fd;
  time[0] = 0;
  time[1] = 10;
  struct State s = {out, set, time, route, action, fd};
  U threads[2];
  if (pthread_create(threads, 0, reader, &s) ||
      pthread_create(threads + 1, 0, other, &s))
    return 2;
  if (pthread_join(threads[0], (void **)(out + 12)) ||
      pthread_join(threads[1], (void **)(out + 13)))
    return 3;
  return 0;
}

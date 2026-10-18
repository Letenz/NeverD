/* Independently authored libc callers. All peers use workload-only contracts.
 */
typedef unsigned long U;
extern int *__errno(void);
extern int socket(int, int, int);
extern int connect(int, const void *, unsigned);
extern int setsockopt(int, int, int, const void *, unsigned);
extern int pthread_create(U *, const void *, void *(*)(void *), void *);
extern int pthread_join(U, void **);
extern int nanosleep(const void *, void *);
extern int clock_gettime(int, void *);
extern void *fdopen(int, const char *);
extern int setvbuf(void *, char *, int, U);
extern U fread(void *, U, U, void *);
extern U __fread_chk(void *, U, U, void *, U);
extern char *__fgets_chk(char *, int, void *, U);
extern U fread_unlocked(void *, U, U, void *);
extern char *fgets(char *, int, void *);
extern char *fgets_unlocked(char *, int, void *);
extern int fgetc(void *);
extern int fgetc_unlocked(void *);
extern int feof(void *);
extern int ferror(void *);
extern int fclose(void *);
static const unsigned char peer[16] = {2, 0, 0x23, 0x29, 127, 0, 0, 1};
struct State {
  U *out;
  U route;
  void *stream;
};
static void *reader(void *arg) {
  struct State *s = arg;
  char *out = (char *)(s->out + 32);
  *__errno() = 73;
  switch (s->route) {
  case 0:
    s->out[0] = fread(out, 2, 3, s->stream);
    break;
  case 1:
    s->out[0] = (U)fgets(out, 16, s->stream);
    break;
  case 2:
    s->out[0] = (unsigned)fgetc(s->stream);
    break;
  case 3:
    s->out[0] = fread_unlocked(out, 2, 3, s->stream);
    break;
  case 4:
    s->out[0] = (U)fgets_unlocked(out, 16, s->stream);
    break;
  case 6:
    s->out[0] = __fread_chk(out, 2, 3, s->stream, 16);
    break;
  case 7:
    s->out[0] = (U)__fgets_chk(out, 16, s->stream, 16);
    break;
  default:
    s->out[0] = (unsigned)fgetc_unlocked(s->stream);
    break;
  }
  s->out[1] = (unsigned)*__errno();
  clock_gettime(1, s->out + 2);
  s->out[4] = feof(s->stream);
  s->out[5] = ferror(s->stream);
  if (s->route == 1 || s->route == 4 || s->route == 7) {
    s->out[6] = (unsigned)fgetc(s->stream);
    s->out[7] = (unsigned)fgetc(s->stream);
    s->out[8] = feof(s->stream);
  }
  return (void *)42;
}
static void *other(void *arg) {
  struct State *s = arg;
  U delay[2] = {0, 3};
  nanosleep(delay, 0);
  *__errno() = 91;
  clock_gettime(1, s->out + 12);
  s->out[14] = 456;
  return (void *)43;
}
U stream_waits(U *out, U route, U capacity) {
  int fd = socket(2, 1, 6);
  if (fd < 0 || connect(fd, peer, 16))
    return 1;
  U timeout[2] = {2, 0};
  if (setsockopt(fd, 1, 20, timeout, 16))
    return 2;
  void *stream = fdopen(fd, "r");
  if (!stream || setvbuf(stream, (char *)(out + 64), 0, capacity))
    return 3;
  struct State s = {out, route, stream};
  U threads[2];
  if (pthread_create(threads, 0, reader, &s) ||
      pthread_create(threads + 1, 0, other, &s))
    return 4;
  if (pthread_join(threads[0], (void **)(out + 10)) ||
      pthread_join(threads[1], (void **)(out + 11)))
    return 5;
  return (unsigned)fclose(stream);
}

U stream_checked_bounds(U *out, U route) {
  if (route == 0)
    return (U)__fgets_chk((char *)out, 16, (void *)1, 15);
  if (route == 1)
    return (U)__fgets_chk((char *)out, -1, (void *)1, ~(U)0);
  return __fread_chk(out, 2, 3, (void *)1, 5);
}

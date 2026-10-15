/* Independently authored API 28 stdio callers; no host libc participates. */
typedef unsigned long u64;
typedef struct FILE FILE;
extern FILE *fopen(const char *, const char *);
extern FILE *fopen64(const char *, const char *);
extern FILE *fdopen(int, const char *);
extern int fclose(FILE *);
extern int fileno(FILE *);
extern int fileno_unlocked(FILE *);
extern int fgetc(FILE *);
extern int getc(FILE *);
extern int getc_unlocked(FILE *);
extern int fgetc_unlocked(FILE *);
extern char *fgets(char *, int, FILE *);
extern char *fgets_unlocked(char *, int, FILE *);
extern u64 fread(void *, u64, u64, FILE *);
extern u64 fread_unlocked(void *, u64, u64, FILE *);
extern int feof(FILE *);
extern int feof_unlocked(FILE *);
extern int ferror(FILE *);
extern int ferror_unlocked(FILE *);
extern void clearerr(FILE *);
extern void clearerr_unlocked(FILE *);
extern int fseek(FILE *, long, int);
extern int fseeko(FILE *, long, int);
extern int fseeko64(FILE *, long, int);
extern long ftell(FILE *);
extern long ftello(FILE *);
extern long ftello64(FILE *);
extern int fgetpos(FILE *, long *);
extern int fgetpos64(FILE *, long *);
extern int fsetpos(FILE *, const long *);
extern int fsetpos64(FILE *, const long *);
extern void rewind(FILE *);
extern int fflush(FILE *);
extern int fflush_unlocked(FILE *);
extern int setvbuf(FILE *, char *, int, u64);
extern void setbuf(FILE *, char *);
extern void setbuffer(FILE *, char *, int);
extern int setlinebuf(FILE *);
extern int open(const char *, int, ...);
extern int close(int);
extern int dup(int);
extern long read(int, void *, u64);
extern long lseek(int, long, int);
extern long syscall(long, ...);
extern int *__errno(void);
extern int mprotect(void *, u64, int);
extern int munmap(void *, u64);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
extern int pthread_create(u64 *, const void *, void *(*)(void *), void *);
extern int pthread_join(u64, void **);

static long raw_seek(int fd, long offset, int origin) {
  register long x0 __asm__("x0") = fd;
  register long x1 __asm__("x1") = offset;
  register long x2 __asm__("x2") = origin;
  register long x8 __asm__("x8") = 62;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x8)
                   : "memory", "cc");
  return x0;
}

u64 stdio_sequence(u64 *out) {
  FILE *f = fopen64("/fixture/input", "rbe");
  char *text = (char *)(out + 64);
  if (!f)
    return 100;
  int fd = fileno(f);
  *__errno() = 717;
  out[0] = fd;
  out[1] = fgetc(f);
  out[2] = raw_seek(fd, 0, 1);
  out[3] = ftell(f);
  out[4] = fgets(text, 16, f) == text;
  out[5] = ftello(f);
  out[6] = fread(text + 16, 3, 3, f);
  out[7] = ftello64(f);
  out[8] = feof(f);
  out[9] = (long)getc(f);
  out[10] = feof_unlocked(f);
  out[11] = ferror_unlocked(f);
  out[12] = fgetpos64(f, (long *)(out + 13));
  out[14] = *__errno();
  out[15] = fclose(f);
  out[16] = (long)read(fd, text + 32, 1);
  out[17] = *__errno();
  return 0;
}

u64 stdio_seek(u64 *out) {
  FILE *f = fopen("/fixture/input", "r");
  char *text = (char *)(out + 64);
  out[0] = getc_unlocked(f);
  out[1] = fseek(f, 2, 1);
  out[2] = ftello(f);
  out[3] = fgetc_unlocked(f);
  out[4] = fseeko64(f, -4, 2);
  out[5] = fgetpos(f, (long *)(out + 6));
  out[7] = fread_unlocked(text, 3, 2, f);
  out[8] = feof(f);
  out[9] = fsetpos64(f, (const long *)(out + 6));
  out[10] = feof(f);
  out[11] = fgets_unlocked(text + 16, 9, f) == text + 16;
  out[12] = feof(f);
  out[13] = fsetpos(f, (const long *)(out + 6));
  out[14] = fseeko(f, -1, 0);
  out[15] = *__errno();
  out[16] = ftell(f);
  out[17] = feof(f);
  out[18] = ferror(f);
  rewind(f);
  out[19] = ftell(f);
  out[20] = feof(f);
  out[21] = ferror(f);
  out[22] = getc(f);
  return fclose(f);
}

u64 stdio_adopt(u64 *out) {
  int fd = open("/fixture/input", 0);
  int alias = dup(fd);
  char *text = (char *)(out + 64);
  out[0] = read(fd, text, 2);
  FILE *f = fdopen(alias, "rbt");
  out[1] = fileno_unlocked(f);
  out[2] = ftell(f);
  out[3] = fgetc(f);
  out[4] = lseek(fd, 0, 1);
  out[5] = ftell(f);
  out[6] = fseek(f, 0, 1);
  out[7] = lseek(fd, 0, 1);
  out[8] = fclose(f);
  out[9] = read(fd, text + 16, 2);
  out[10] = (long)read(alias, text + 32, 1);
  out[11] = *__errno();
  FILE *other = fopen("/fixture/input", "r");
  out[12] = fgetc(other);
  out[13] = close(fd);
  return fclose(other);
}

u64 stdio_eof_retry(u64 *out) {
  FILE *f = fopen("/fixture/input", "r");
  int fd = fileno(f);
  out[0] = fseek(f, 0, 2);
  out[1] = (long)fgetc(f);
  out[2] = feof(f);
  out[3] = lseek(fd, 0, 0);
  out[4] = fgetc(f); /* API 28 retries despite a sticky EOF indicator. */
  out[5] = feof(f);
  out[6] = ftell(f);
  clearerr_unlocked(f);
  out[7] = feof(f);
  out[8] = fflush(f);
  out[9] = *__errno();
  out[10] = ftell(f);
  out[11] = ferror(f);
  out[12] = fflush_unlocked(f);
  out[13] = fflush(0);
  out[14] = fgetc(f);
  return fclose(f);
}

u64 stdio_closed_fd(u64 *out, u64 after_read) {
  FILE *f = fopen("/fixture/input", "r");
  int fd = fileno(f);
  if (after_read)
    out[0] = fgetc(f);
  out[1] = close(fd);
  if (after_read)
    out[2] = fread((char *)(out + 64), 1, 7, f);
  out[3] = (long)fgetc(f);
  out[4] = ferror(f);
  out[5] = feof(f);
  out[6] = *__errno();
  out[7] = (long)ftell(f);
  out[8] = fileno(f);
  clearerr(f);
  out[9] = ferror(f);
  out[10] = fclose(f);
  out[11] = *__errno();
  return 0;
}

u64 stdio_transfer(u64 *out, u64 size, u64 count, char *buffer) {
  FILE *f = fopen("/fixture/input", "r");
  *__errno() = 717;
  out[0] = fread(buffer, size, count, f);
  out[1] = ftell(f);
  out[2] = feof(f);
  out[3] = ferror(f);
  out[4] = *__errno();
  return fclose(f);
}

u64 stdio_line(u64 *out, int n, char *buffer) {
  FILE *f = fopen("/fixture/input", "r");
  *__errno() = 717;
  out[0] = fgets(buffer, n, f) == buffer;
  out[1] = ftell(f);
  out[2] = feof(f);
  out[3] = ferror(f);
  out[4] = *__errno();
  return fclose(f);
}

u64 stdio_buffering(u64 *out, int kind, char *buffer, u64 size) {
  FILE *f = fopen("/fixture/input", "r");
  *__errno() = 717;
  out[0] = setvbuf(f, buffer, kind, size);
  out[1] = fgetc(f);
  out[2] = lseek(fileno(f), 0, 1);
  out[3] = ftell(f);
  out[4] = *__errno();
  out[5] = ferror(f);
  return fclose(f);
}

u64 stdio_rebuffer(u64 *out, u64 kind) {
  FILE *f = fopen("/fixture/input", "r");
  char *text = (char *)(out + 64);
  out[0] = fgetc(f);
  if (kind == 0)
    setbuf(f, 0);
  else if (kind == 1)
    setbuffer(f, text, 3);
  else
    out[1] = setlinebuf(f);
  out[2] = ftell(f); /* Discard unread input without rewinding the FD. */
  out[3] = fgetc(f);
  out[4] = lseek(fileno(f), 0, 1);
  out[5] = ftell(f);
  return fclose(f);
}

u64 stdio_open_case(u64 *out, const char *mode, u64 adopt, int fd) {
  *__errno() = 717;
  FILE *f = adopt ? fdopen(fd, mode) : fopen("/fixture/input", mode);
  out[0] = f != 0;
  out[1] = *__errno();
  if (f) {
    out[2] = fileno(f);
    out[3] = fclose(f);
  }
  return 0;
}

u64 stdio_bad_handle(u64 kind) {
  FILE *f = fopen("/fixture/input", "r");
  if (kind == 0)
    fclose(f);
  if (kind == 1)
    f = (FILE *)((char *)f + 1);
  if (kind == 2)
    mprotect(f, 4096, 3);
  if (kind == 3)
    munmap(f, 4096);
  if (kind == 4)
    return *(volatile unsigned char *)f;
  return (u64)ftell(f);
}

struct shared {
  FILE *f;
  u64 *out;
};
static void *stdio_child(void *arg) {
  struct shared *s = arg;
  s->out[1] = *__errno();
  s->out[2] = fgetc(s->f);
  s->out[3] = ftell(s->f);
  s->out[4] = fflush(s->f);
  s->out[5] = *__errno();
  return (void *)99;
}
u64 stdio_threads(u64 *out) {
  struct shared s = {fopen("/fixture/input", "r"), out};
  u64 t;
  out[0] = fgetc(s.f);
  *__errno() = 717;
  out[6] = pthread_create(&t, 0, stdio_child, &s);
  out[7] = pthread_join(t, (void **)(out + 8));
  out[9] = fgetc(s.f);
  out[10] = ftell(s.f);
  out[11] = *__errno();
  return fclose(s.f);
}

u64 stdio_dynamic(u64 *out, u64 inactive) {
  void *h = dlopen("libstreams.so", 2);
  FILE *(*op)(const char *, const char *) = dlsym(h, "fopen");
  int (*gc)(FILE *) = dlsym(h, "fgetc");
  int (*cl)(FILE *) = dlsym(h, "fclose");
  FILE *f = op("/fixture/input", "r");
  if (inactive)
    dlclose(h);
  out[0] = gc(f);
  out[1] = ftell(f);
  out[2] = cl(f);
  return 0;
}

extern void *malloc(u64);
extern void free(void *);
u64 stdio_allocation_lifetime(u64 *out) {
  void *held[512];
  u64 count = 0;
  while (count < 512 && (held[count] = malloc(4096)))
    ++count;
  if (count == 512)
    return 100;
  out[0] = fopen("/fixture/input", "r") == 0;
  out[1] = *__errno();
  int fd = open("/fixture/input", 0);
  out[2] = fd;
  out[3] = fdopen(fd, "r") == 0;
  out[4] = *__errno();
  out[5] = lseek(fd, 0, 1);
  out[6] = close(fd);
  while (count)
    free(held[--count]);
  for (unsigned i = 0; i != 100; ++i) {
    FILE *f = fopen("/fixture/input", "r");
    if (!f || fgetc(f) != 'a' || fclose(f))
      return 101;
    ++out[7];
  }
  return 0;
}

u64 stdio_error_indicators(u64 *out) {
  FILE *f = fopen("/fixture/input", "r");
  out[0] = fread((void *)1, ~0UL, 2, f);
  out[1] = ferror(f);
  out[2] = fseek(f, 0, 2);
  out[3] = (long)fgetc(f);
  out[4] = ferror(f);
  out[5] = feof(f);
  out[6] = fseek(f, 0, 0);
  out[7] = ferror(f);
  out[8] = feof(f);
  out[9] = fgetc(f);
  out[10] = ferror(f);
  rewind(f);
  out[11] = ferror(f);
  out[12] = feof(f);
  return fclose(f);
}
